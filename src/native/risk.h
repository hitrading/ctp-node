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

namespace ctp {

struct RiskConfig {
  int maxOrderVolume = 0;          // per single order; 0 = disabled
  double maxPriceDeviation = 0.0;  // |px-ref|/ref ratio; 0 = disabled
  double maxNotional = 0.0;        // price * volume; 0 = disabled
  double maxOrdersPerSec = 0.0;    // token-bucket rate; 0 = disabled
  double orderBurst = 0.0;         // bucket burst; <=0 -> defaults to rate
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

  // Validation only, no side effects. refPrice<=0 skips deviation/notional.
  RiskVerdict check(double price, double refPrice, int volume) const;

  // Rate gate (consumes a token). Call right before sending.
  bool allowRate() { return limiter_.tryAcquire(); }

  // TODO(position): per-instrument net-position limits need live position
  //   state from the trade-return path (not built yet). Reserved hook.

private:
  std::atomic<bool> halted_{false};
  std::atomic<int> maxOrderVolume_{0};
  std::atomic<double> maxPriceDeviation_{0.0};
  std::atomic<double> maxNotional_{0.0};
  RateLimiter limiter_;
};

} // namespace ctp

#endif /* CTP_NATIVE_RISK_H */
