/*
 * risk.h - pre-trade risk enforcement + rate limiting, sunk into C++.
 *
 * These run on the HOT path (right before an order is sent - which may be the
 * JS thread today, or the CTP callback thread once armed triggers fire). They
 * must be deterministic and never blocked by a JS GC pause, so config is held
 * in atomics, published from the slow path (JS) and read lock-free on the hot
 * path. JS sets the rules; C++ enforces them on every order.
 */

#ifndef CTP_NATIVE_RISK_H
#define CTP_NATIVE_RISK_H

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ctp {

// A read-only source of latest reference prices (the MarketData snapshot cache
// implements it). Lets the risk engine read live prices for the deviation check
// in C++ — attached via setMdSnapshot — with no dependency on the CTP md struct
// and no JS round-trip. last() returns 0 for an unknown/absent instrument.
struct RefPriceSource {
  virtual double last(const std::string &instrumentId) const = 0;
  virtual ~RefPriceSource() = default;
};

// One working open order, for rebuilding reservations from reqQryOrder.
struct OpenOrderInfo {
  int frontId = 0;
  int sessionId = 0;
  std::string orderRef;
  std::string instrumentId;
  bool isLong = true;
  double vol = 0.0;   // working remainder (VolumeTotalOriginal - VolumeTraded)
  double price = 0.0; // LimitPrice
};

struct RiskConfig {
  int maxOrderVolume = 0;          // per single order; 0 = disabled
  double maxPriceDeviation = 0.0;  // |px-ref|/ref ratio; 0 = disabled
  double maxNotional = 0.0;        // price * volume; 0 = disabled
  double maxOrdersPerSec = 0.0;    // token-bucket rate; 0 = disabled
  double orderBurst = 0.0;         // bucket burst; <=0 -> defaults to rate
  double maxPositionCost = 0.0;    // DEPRECATED alias for maxMargin; 0 = off
  double maxMargin = 0.0;          // cap on total open-position MARGIN
                                   // (Σ price*vol*mult*marginRate); 0 = off.
                                   // Takes precedence over maxPositionCost.
};

struct RiskVerdict {
  bool ok = true;
  const char *reason = nullptr;    // static string; null when ok
};

// Result of the position gate for an opening order.
enum class OpenGate { Ok, CostLimit, VolumeLimit };

// Token-bucket rate limiter. Thread-safe via a tiny mutex; rate<=0 disables.
class RateLimiter {
public:
  void configure(double ratePerSec, double burst);
  bool tryAcquire();               // true = allowed, false = throttled

private:
  std::mutex m_;
  double rate_ = 0.0;
  double burst_ = 0.0;
  double tokens_ = 0.0;
  std::chrono::steady_clock::time_point last_{};
  bool initialized_ = false;
};

class RiskEngine {
public:
  void configure(const RiskConfig &cfg); // slow path (JS thread)
  void halt();                            // kill-switch on
  void resume();                          // kill-switch off
  bool isHalted() const { return halted_.load(std::memory_order_relaxed); }

  // Validation only (no position side effects). refPrice<=0 skips deviation;
  // the instrument's multiplier (if set) is applied to the notional check.
  RiskVerdict check(const std::string &instrumentId, double price,
                    double refPrice, int volume) const;
  // Static reason string of the most recent check() rejection (for diagnostics
  // / JS-side logging); null if none. Cheap, set only on the reject path.
  const char *lastBlockReason() const { return lastReason_.load(std::memory_order_relaxed); }

  // Rate gate (consumes a token). Call right before sending.
  bool allowRate() { return limiter_.tryAcquire(); }

  // Attach a live reference-price source (the MarketData snapshot cache) so the
  // maxPriceDeviation check reads current prices in C++ — straight from the MD
  // feed, no JS round-trip. Pass nullptr to detach. Without one, refPrice()
  // returns 0 and the deviation check is skipped.
  void setMdSnapshot(std::shared_ptr<const RefPriceSource> source);
  // Reference price for the deviation check: the attached MD snapshot's last
  // price, or 0 if none/unknown.
  double refPrice(const std::string &instrumentId) const;

  // Max total open-position MARGIN (Σ open price*volume*multiplier*marginRate).
  // Updated by fills (onTrade) on the trader callback thread; seed pre-existing
  // positions with seedPosition. Set the contract multiplier AND margin rate for
  // accuracy.
  void setMultiplier(const std::string &instrumentId, double multiplier);
  // Per-instrument margin rate (fraction of notional, e.g. 0.10 = 10%), fed
  // directly from CTP's OnRspQryInstrumentMarginRate inside C++ (no JS round-
  // trip). Makes the tracked open "cost" real MARGIN, so the global cap
  // (configure().maxMargin) and the per-instrument cost cap are margin limits.
  // Unset/invalid -> a conservative default (kDefaultMarginRate) applies, so the
  // cap over-estimates (stays safe) until the real rate arrives.
  void setMarginRate(const std::string &instrumentId, double rate);
  void seedPosition(const std::string &instrumentId, bool isLong, double volume, double cost);
  void onTrade(const std::string &instrumentId, bool isBuy, bool isOpen, double price, double volume);
  void resetPositions();
  double currentPositionCost() const;
  // Per-instrument cap on open-position cost (Σ price*volume*multiplier, both
  // sides summed - a gross capital/concentration limit). 0/unset = no cap.
  // Complements the global maxPositionCost.
  void setMaxInstrumentCost(const std::string &instrumentId, double maxCost);
  // Per-instrument, per-side cap on open position volume (lots). Long and short
  // capped independently; 0/unset = that side unlimited.
  void setMaxPositionVolume(const std::string &instrumentId, bool isLong, double maxVolume);

  // Our login session (FrontID, SessionID), set on rsp-user-login. Reservations
  // are keyed by (front, session, orderRef) so orders from THIS session never
  // collide with another terminal's orders on the same account (which CTP also
  // delivers to us, and which auto-seed the same orderRef numbers).
  void setSession(int frontId, int sessionId);

  // Atomically check ALL position caps (per-side volume, per-instrument cost,
  // global cost) against committed = held + in-flight reserved, and, if the
  // order passes and any cap applies, reserve it (under our own session key) so
  // a burst of opens can't slip past before fills arrive. Returns the cap that
  // blocked, or Ok (reserved). Pair every Ok with an eventual release: a fill/
  // cancel/reject reconciles via onOrderUpdate / releaseReservation, and a
  // failed send (api rc != 0) must call releaseReservation - no lifecycle event.
  OpenGate tryReserveOpen(const std::string &orderRef, const std::string &instrumentId,
                          bool isLong, double price, double volume);
  // Reconcile an open order's reservation to its current working remainder
  // (volTotal - volTraded while queueing; 0 once terminal). Fed from OnRtnOrder,
  // which carries the originating session - so this also tracks (and releases)
  // orders placed by another terminal on the same account.
  void onOrderUpdate(int frontId, int sessionId, const std::string &orderRef,
                     const std::string &instrumentId, bool isOpen, bool isLong,
                     char status, double limitPrice, double volTotal, double volTraded);
  // Drop one of OUR orders' reservations outright (front/insert rejection, or
  // failed send - neither yields an OnRtnOrder).
  void releaseReservation(const std::string &orderRef);
  // Rebuild all in-flight reservations from CTP's working orders (reqQryOrder).
  // Authoritative resync for (re)connect; clears prior reservations first.
  void rebuildOpenReservations(const std::vector<OpenOrderInfo> &orders);

private:
  std::atomic<bool> halted_{false};
  std::atomic<int> maxOrderVolume_{0};
  std::atomic<double> maxPriceDeviation_{0.0};
  std::atomic<double> maxNotional_{0.0};
  std::atomic<double> maxPositionCost_{0.0};
  RateLimiter limiter_;
  // Live reference-price source (the MarketData snapshot cache), read by
  // refPrice() for the deviation check. Guarded by refMutex_.
  mutable std::mutex refMutex_;
  std::shared_ptr<const RefPriceSource> mdSnap_;

  struct Pos {
    double longVol = 0, longCost = 0, shortVol = 0, shortCost = 0;       // filled
    double longPendVol = 0, longPendCost = 0, shortPendVol = 0, shortPendCost = 0; // in-flight reserved
  };
  mutable std::mutex posMutex_;
  std::unordered_map<std::string, Pos> positions_;
  int myFrontId_ = 0, mySessionId_ = 0; // our session, for reservation keys
  // In-flight open-order reservations, keyed by (front:session:orderRef) so a
  // fill/cancel/reject releases exactly what each order reserved (its working
  // remainder), and our orders never collide with another terminal's.
  struct Reservation {
    std::string instrumentId;
    bool isLong = true;
    double vol = 0.0, cost = 0.0;
  };
  std::unordered_map<std::string, Reservation> reservations_;
  static std::string resvKey(int frontId, int sessionId, const std::string &orderRef);
  // Contract multipliers are static metadata kept separate from position state
  // so resetPositions() (which syncPositions calls) does not wipe them.
  std::unordered_map<std::string, double> multipliers_;
  // Per-instrument margin rates (fraction of notional), same lifetime as
  // multipliers_ (static metadata; survives resetPositions). Unset -> default.
  std::unordered_map<std::string, double> marginRate_;
  // Fallback when an instrument's rate is unset: full notional (1.0). Maximally
  // conservative (margin <= notional always), so an un-fetched contract can only
  // OVER-count its margin — the cap never under-blocks — and the cost cap stays
  // back-compatible (no rate set == the previous notional behavior).
  static constexpr double kDefaultMarginRate = 1.0;
  // Per-instrument, per-side lot caps (config metadata; like multipliers,
  // survives resets). 0 on a side = that side is uncapped.
  struct VolCap {
    double longMax = 0.0, shortMax = 0.0;
  };
  std::unordered_map<std::string, VolCap> maxPositionVol_;
  // Per-instrument open-cost caps (config metadata; survives resets). 0 = off.
  std::unordered_map<std::string, double> maxInstrumentCost_;
  // Look up an instrument's multiplier (1.0 if unset). Caller holds posMutex_.
  double multiplierLocked(const std::string &instrumentId) const;
  // Margin rate (kDefaultMarginRate if unset). Caller holds posMutex_.
  double marginRateLocked(const std::string &instrumentId) const;
  // Factor converting price*volume into tracked MARGIN: multiplier * marginRate.
  // Used for all open-position cost accounting. Caller holds posMutex_.
  double costFactorLocked(const std::string &instrumentId) const;
  // Whether any position cap applies to this instrument/side. Holds posMutex_.
  bool hasCapLocked(const std::string &instrumentId, bool isLong) const;
  // Record + return a check() rejection (stores the reason for lastBlockReason).
  RiskVerdict block(const char *reason) const;
  mutable std::atomic<const char *> lastReason_{nullptr};
};

} // namespace ctp

#endif /* CTP_NATIVE_RISK_H */
