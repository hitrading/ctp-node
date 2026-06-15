/*
 * arm.h - latency-critical "armed" triggers, evaluated in C++ on the CTP
 * callback thread and fired the instant their condition hits, WITHOUT crossing
 * into JS. JS only arms/disarms (slow path) and is notified after the fact.
 *
 * This is the "sunk-down inner loop" hook. The registry + matching are real;
 * the actual order fire is reserved (TODO) until the Trader send path exists.
 */

#ifndef CTP_NATIVE_ARM_H
#define CTP_NATIVE_ARM_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ctp {

struct ArmSpec {
  uint64_t id = 0;
  std::string instrumentId;
  char side = '0';                 // CTP Direction: '0' buy, '1' sell
  double triggerPrice = 0.0;
  int volume = 0;
  double cancelIfWorseThan = 0.0;  // 0 = unused
};

class ArmRegistry {
public:
  uint64_t arm(const ArmSpec &spec); // slow path (JS)
  bool disarm(uint64_t id);          // slow path (JS)
  size_t size() const;

  // Hot path: CTP callback thread, once per depth tick. Evaluates triggers for
  // the given instrument and (TODO) fires matches inline.
  void onTick(const char *instrumentId, double bid, double ask);

  // TODO(fire):   wire to the Trader order-send path (doesn't exist yet):
  //                 setFireCallback(std::function<void(const ArmSpec&,double)>)
  // TODO(notify): async-notify JS after a fire via a ThreadSafeFunction.
  // TODO(perf):   replace the mutex with a read-mostly lock-free snapshot so
  //                 onTick never blocks on rare arm/disarm writes.

private:
  mutable std::mutex m_;
  std::vector<ArmSpec> armed_;
  uint64_t nextId_ = 1;
};

} // namespace ctp

#endif /* CTP_NATIVE_ARM_H */
