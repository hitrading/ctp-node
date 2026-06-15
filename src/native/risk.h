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
  // True if opening this order keeps the total open cost within maxPositionCost.
  bool allowOpen(const std::string &instrumentId, double price, double volume) const;

  // Per-instrument cap on open position volume (lots), enforced per side on
  // open orders (long and short tracked independently). 0/unset = unlimited.
  void setMaxPositionVolume(const std::string &instrumentId, double maxVolume);
  // True if opening `volume` more lots on this side stays within the cap.
  bool allowOpenVolume(const std::string &instrumentId, bool isLong, double volume) const;

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
    double longVol = 0, longCost = 0, shortVol = 0, shortCost = 0;
  };
  mutable std::mutex posMutex_;
  std::unordered_map<std::string, Pos> positions_;
  // Contract multipliers are static metadata kept separate from position state
  // so resetPositions() (which syncPositions calls) does not wipe them.
  std::unordered_map<std::string, double> multipliers_;
  // Per-instrument lot caps (config metadata; like multipliers, survives resets).
  std::unordered_map<std::string, double> maxPositionVol_;
  // Look up an instrument's multiplier (1.0 if unset). Caller holds posMutex_.
  double multiplierLocked(const std::string &instrumentId) const;
};

} // namespace ctp

#endif /* CTP_NATIVE_RISK_H */
