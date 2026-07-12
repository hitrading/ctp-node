/*
 * arm.h - latency-critical "armed" triggers.
 *
 * An armed order is evaluated on the MarketData callback thread on every depth
 * tick and, the instant its trigger hits, fired through the Trader (pre-trade
 * risk + ReqOrderInsert) WITHOUT crossing into JS. The order acknowledgement
 * flows back through the normal trader events (rtn-order / rsp-order-insert);
 * correlate via the order's OrderRef.
 *
 * Lifetime: the registry is shared (shared_ptr) between the Trader that owns it
 * and any MarketData feeding it ticks, so neither side can dangle. The Trader
 * clears the sink (under the registry lock) before releasing its API, so no
 * fire can race a teardown.
 */

#ifndef CTP_NATIVE_ARM_H
#define CTP_NATIVE_ARM_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ctp {

struct ArmSpec {
  uint64_t id = 0;
  std::string instrumentId;
  char side = '0'; // '0' buy: fire when ask<=trigger; '1' sell: bid>=trigger
  double triggerPrice = 0.0;
  std::vector<uint8_t> orderTemplate; // pre-encoded CThostFtdcInputOrderField
  int lastRc = 0;
  std::chrono::steady_clock::time_point nextAttempt{};
  uint32_t retryDelayMs = 100;
  bool firing = false;
};

// Implemented by the Trader. Called on the MD callback thread; must be
// thread-safe (CTP request methods are).
struct OrderSink {
  virtual ~OrderSink() {}
  virtual int fireArmed(const ArmSpec &spec) = 0;
};

class ArmRegistry {
public:
  void setSink(OrderSink *sink); // link (Trader ctor)
  void clearSink();              // unlink under lock (Trader teardown)
  bool hasSink() const;          // safe attach/replacement check
  void clear();                  // remove every arm, keep the Trader sink linked

  uint64_t arm(ArmSpec spec); // JS thread
  bool disarm(uint64_t id);   // JS thread
  size_t size() const;
  // Armed triggers that were successfully SENT (fireArmed returned 0).
  uint64_t fireCount() const { return fireCount_.load(std::memory_order_relaxed); }
  // Attempts refused by the risk gate / send (rc!=0). Refused triggers remain
  // armed and retry with bounded exponential backoff; this is an attempt counter.
  uint64_t blockedCount() const { return blockedCount_.load(std::memory_order_relaxed); }

  // Hot path: MD callback thread, once per depth tick.
  void onTick(const char *instrumentId, double bid, double ask);

private:
  mutable std::mutex m_;
  std::condition_variable idleCv_;
  size_t inFlight_ = 0;
  std::unordered_map<std::string, std::vector<ArmSpec>> armedByInstrument_;
  std::unordered_map<uint64_t, std::string> instrumentById_;
  uint64_t nextId_ = 1;
  std::atomic<size_t> count_{0}; // lock-free "any arms?" gate
  std::atomic<uint64_t> fireCount_{0};
  std::atomic<uint64_t> blockedCount_{0};
  OrderSink *sink_ = nullptr; // guarded by m_
};

} // namespace ctp

#endif /* CTP_NATIVE_ARM_H */
