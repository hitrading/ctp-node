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
#include <mutex>
#include <string>
#include <unordered_map>

namespace ctp {

struct RiskConfig {
  int maxOrderVolume = 0;          // per single order; 0 = disabled
  double maxPriceDeviation = 0.0;  // |px-ref|/ref ratio; 0 = disabled
  double maxNotional = 0.0;        // price * volume; 0 = disabled
  double maxOrdersPerSec = 0.0;    // token-bucket rate; 0 = disabled
  double orderBurst = 0.0;         // bucket burst; <=0 -> defaults to rate
  double maxPositionCost = 0.0;    // cap on total open-position cost; 0 = off
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

  // Validation only, no side effects. refPrice<=0 skips deviation; the
  // instrument's multiplier (if set) is applied to the notional check.
  RiskVerdict check(const std::string &instrumentId, double price,
                    double refPrice, int volume) const;

  // Rate gate (consumes a token). Call right before sending.
  bool allowRate() { return limiter_.tryAcquire(); }

  // Per-instrument reference price for the maxPriceDeviation check. Fed from
  // market data (cold-ish path); 0 = unknown -> deviation check skipped.
  void setRefPrice(const std::string &instrumentId, double price);
  double refPrice(const std::string &instrumentId) const;

  // Max total open-position cost (sum of open price*volume*multiplier). Updated
  // by fills (onTrade) on the trader callback thread; seed pre-existing
  // positions with seedPosition. Set the contract multiplier for accuracy.
  void setMultiplier(const std::string &instrumentId, double multiplier);
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

  // Atomically check ALL position caps (per-side volume, per-instrument cost,
  // global cost) against committed = held + in-flight reserved, and, if the
  // order passes and any cap applies, reserve it (keyed by orderRef) so a burst
  // of opens can't slip past before fills arrive. Returns the cap that blocked,
  // or Ok (reserved). Pair every Ok with an eventual release: a fill/cancel/
  // reject reconciles via onOrderUpdate / releaseReservation, and a failed send
  // (api rc != 0) must call releaseReservation since no lifecycle event will.
  OpenGate tryReserveOpen(const std::string &orderRef, const std::string &instrumentId,
                          bool isLong, double price, double volume);
  // Reconcile an open order's reservation to its current working remainder
  // (volTotal - volTraded while queueing; 0 once terminal). Fed from OnRtnOrder.
  void onOrderUpdate(const std::string &orderRef, const std::string &instrumentId,
                     bool isOpen, bool isLong, char status, double limitPrice,
                     double volTotal, double volTraded);
  // Drop an order's reservation outright (front/insert rejection, or failed send).
  void releaseReservation(const std::string &orderRef);

private:
  std::atomic<bool> halted_{false};
  std::atomic<int> maxOrderVolume_{0};
  std::atomic<double> maxPriceDeviation_{0.0};
  std::atomic<double> maxNotional_{0.0};
  std::atomic<double> maxPositionCost_{0.0};
  RateLimiter limiter_;
  mutable std::mutex refMutex_;
  std::unordered_map<std::string, double> refPrices_;

  struct Pos {
    double longVol = 0, longCost = 0, shortVol = 0, shortCost = 0;       // filled
    double longPendVol = 0, longPendCost = 0, shortPendVol = 0, shortPendCost = 0; // in-flight reserved
  };
  mutable std::mutex posMutex_;
  std::unordered_map<std::string, Pos> positions_;
  // In-flight open-order reservations, keyed by orderRef, so a fill/cancel/
  // reject can release exactly what each order reserved (its working remainder).
  struct Reservation {
    std::string instrumentId;
    bool isLong = true;
    double vol = 0.0, cost = 0.0;
  };
  std::unordered_map<std::string, Reservation> reservations_;
  // Contract multipliers are static metadata kept separate from position state
  // so resetPositions() (which syncPositions calls) does not wipe them.
  std::unordered_map<std::string, double> multipliers_;
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
  // Whether any position cap applies to this instrument/side. Holds posMutex_.
  bool hasCapLocked(const std::string &instrumentId, bool isLong) const;
};

} // namespace ctp

#endif /* CTP_NATIVE_RISK_H */
