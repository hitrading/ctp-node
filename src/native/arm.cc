/*
 * arm.cc - see arm.h
 */

#include "arm.h"

namespace ctp {

void ArmRegistry::setSink(OrderSink *sink) {
  std::lock_guard<std::mutex> lk(m_);
  sink_ = sink;
}

void ArmRegistry::clearSink() {
  std::lock_guard<std::mutex> lk(m_);
  sink_ = nullptr;
}

uint64_t ArmRegistry::arm(ArmSpec spec) {
  std::lock_guard<std::mutex> lk(m_);
  spec.id = nextId_++;
  spec.fired = false;
  armed_.push_back(std::move(spec));
  count_.store(armed_.size(), std::memory_order_relaxed);
  return armed_.back().id;
}

bool ArmRegistry::disarm(uint64_t id) {
  std::lock_guard<std::mutex> lk(m_);
  for (auto it = armed_.begin(); it != armed_.end(); ++it) {
    if (it->id == id) {
      armed_.erase(it);
      count_.store(armed_.size(), std::memory_order_relaxed);
      return true;
    }
  }
  return false;
}

size_t ArmRegistry::size() const {
  std::lock_guard<std::mutex> lk(m_);
  return armed_.size();
}

void ArmRegistry::onTick(const char *instrumentId, double bid, double ask) {
  if (count_.load(std::memory_order_relaxed) == 0)
    return; // no arms: zero overhead on the tick hot path

  std::lock_guard<std::mutex> lk(m_);
  if (!sink_)
    return;

  for (auto &a : armed_) {
    if (a.fired || a.instrumentId != instrumentId)
      continue;
    const bool hit = (a.side == '0') ? (ask > 0.0 && ask <= a.triggerPrice)
                                     : (bid > 0.0 && bid >= a.triggerPrice);
    if (!hit)
      continue;
    a.fired = true; // one-shot
    fireCount_.fetch_add(1, std::memory_order_relaxed);
    sink_->fireArmed(a); // build order + risk + ReqOrderInsert, on this thread
  }
}

} // namespace ctp
