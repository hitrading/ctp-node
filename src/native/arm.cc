/*
 * arm.cc - see arm.h
 */

#include "arm.h"

#include <algorithm>

namespace ctp {

void ArmRegistry::setSink(OrderSink *sink) {
  std::lock_guard<std::mutex> lk(m_);
  sink_ = sink;
}

void ArmRegistry::clearSink() {
  std::unique_lock<std::mutex> lk(m_);
  sink_ = nullptr;
  armedByInstrument_.clear();
  instrumentById_.clear();
  count_.store(0, std::memory_order_release);
  idleCv_.wait(lk, [&] { return inFlight_ == 0; });
}

bool ArmRegistry::hasSink() const {
  std::lock_guard<std::mutex> lk(m_);
  return sink_ != nullptr;
}

void ArmRegistry::clear() {
  std::lock_guard<std::mutex> lk(m_);
  armedByInstrument_.clear();
  instrumentById_.clear();
  count_.store(0, std::memory_order_release);
}

uint64_t ArmRegistry::arm(ArmSpec spec) {
  std::lock_guard<std::mutex> lk(m_);
  spec.id = nextId_++;
  spec.lastRc = 0;
  spec.nextAttempt = {};
  spec.retryDelayMs = 100;
  spec.firing = false;
  const uint64_t id = spec.id;
  const std::string instrument = spec.instrumentId;
  armedByInstrument_[instrument].push_back(std::move(spec));
  instrumentById_[id] = instrument;
  count_.store(instrumentById_.size(), std::memory_order_release);
  return id;
}

bool ArmRegistry::disarm(uint64_t id) {
  std::lock_guard<std::mutex> lk(m_);
  auto idx = instrumentById_.find(id);
  if (idx == instrumentById_.end())
    return false;
  auto bucket = armedByInstrument_.find(idx->second);
  if (bucket != armedByInstrument_.end()) {
    auto &arms = bucket->second;
    for (auto it = arms.begin(); it != arms.end(); ++it) {
      if (it->id == id) {
        arms.erase(it);
        break;
      }
    }
    if (arms.empty())
      armedByInstrument_.erase(bucket);
  }
  instrumentById_.erase(idx);
  count_.store(instrumentById_.size(), std::memory_order_release);
  return true;
}

size_t ArmRegistry::size() const {
  std::lock_guard<std::mutex> lk(m_);
  return instrumentById_.size();
}

void ArmRegistry::onTick(const char *instrumentId, double bid, double ask) {
  if (count_.load(std::memory_order_acquire) == 0)
    return; // no arms: zero overhead on the tick hot path

  const std::string instrument = instrumentId ? instrumentId : "";
  for (;;) {
    ArmSpec attempt;
    OrderSink *sink = nullptr;
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lk(m_);
      if (!sink_)
        return;
      auto bucket = armedByInstrument_.find(instrument);
      if (bucket == armedByInstrument_.end())
        return;
      ArmSpec *candidate = nullptr;
      for (ArmSpec &a : bucket->second) {
        if (a.firing ||
            (a.nextAttempt.time_since_epoch().count() != 0 &&
             now < a.nextAttempt))
          continue;
    // The bid/ask must be a real, finite, sub-sentinel price. CTP writes DBL_MAX
    // (~1.8e308) into BidPrice1/AskPrice1 when a side is empty - e.g. at
    // limit-down there are no bids, so BidPrice1 == DBL_MAX. Without the
    // `< 1e300` guard a SELL trigger (bid >= triggerPrice) would fire on that
    // sentinel: a spurious market sell at the worst possible moment. Mirrors the
    // sentinel band rejected everywhere in risk.cc (sanePositive).
        const bool hit = (a.side == '0')
            ? (ask > 0.0 && ask < 1e300 && ask <= a.triggerPrice)
            : (bid > 0.0 && bid < 1e300 && bid >= a.triggerPrice);
        if (hit) {
          candidate = &a;
          break;
        }
      }
      if (!candidate)
        return;
      candidate->firing = true;
      attempt = *candidate;
      sink = sink_;
      ++inFlight_;
    }
    // One-shot means one successful submission. A transient halt, rate limit, or
    // metadata outage must not silently consume the mandate; keep it armed and
    // retry no faster than every 100ms while the condition remains true.
    // Never hold the registry mutex across a vendor request. A provider may
    // synchronously emit a front callback which clears this same registry.
    const int rc = sink->fireArmed(attempt);
    {
      std::lock_guard<std::mutex> lk(m_);
      --inFlight_;
      idleCv_.notify_all();
      auto idx = instrumentById_.find(attempt.id);
      if (idx == instrumentById_.end())
        continue; // disarmed/session-cleared while the request was in flight
      auto bucket = armedByInstrument_.find(idx->second);
      if (bucket == armedByInstrument_.end())
        continue;
      auto &arms = bucket->second;
      auto it = std::find_if(arms.begin(), arms.end(),
                             [&](const ArmSpec &a) { return a.id == attempt.id; });
      if (it == arms.end())
        continue;
      it->firing = false;
      it->lastRc = rc;
      if (rc == 0) {
      fireCount_.fetch_add(1, std::memory_order_relaxed);
      instrumentById_.erase(attempt.id);
      arms.erase(it); // remove from this instrument's hot bucket immediately
      count_.store(instrumentById_.size(), std::memory_order_release);
      } else {
      blockedCount_.fetch_add(1, std::memory_order_relaxed);
      it->nextAttempt = now + std::chrono::milliseconds(it->retryDelayMs);
      it->retryDelayMs = std::min<uint32_t>(5000, it->retryDelayMs * 2);
      }
      if (arms.empty())
        armedByInstrument_.erase(bucket);
    }
  }
}

} // namespace ctp
