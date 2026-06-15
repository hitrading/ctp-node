/*
 * arm.cc - see arm.h
 */

#include "arm.h"

namespace ctp {

uint64_t ArmRegistry::arm(const ArmSpec &spec) {
  std::lock_guard<std::mutex> lk(m_);
  ArmSpec s = spec;
  s.id = nextId_++;
  armed_.push_back(std::move(s));
  return armed_.back().id;
}

bool ArmRegistry::disarm(uint64_t id) {
  std::lock_guard<std::mutex> lk(m_);
  for (auto it = armed_.begin(); it != armed_.end(); ++it) {
    if (it->id == id) {
      armed_.erase(it);
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
  std::lock_guard<std::mutex> lk(m_);
  for (const auto &a : armed_) {
    if (a.instrumentId != instrumentId)
      continue;

    // Buy fires when the ask drops to/below the trigger; sell when the bid
    // rises to/above it.
    const bool hit = (a.side == '0') ? (ask > 0.0 && ask <= a.triggerPrice)
                                     : (bid > 0.0 && bid >= a.triggerPrice);
    if (!hit)
      continue;

    // TODO(fire): run RiskEngine::check + allowRate, then send the order here,
    //             entirely on this thread (no JS round trip). Reserved.
  }
}

} // namespace ctp
