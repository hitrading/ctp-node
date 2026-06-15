/*
 * risk.cc - see risk.h
 */

#include "risk.h"
#include <algorithm>
#include <cmath>

namespace ctp {

void RateLimiter::configure(double ratePerSec, double burst) {
  std::lock_guard<std::mutex> lk(m_);
  rate_ = ratePerSec > 0.0 ? ratePerSec : 0.0;
  burst_ = burst > 0.0 ? burst : rate_; // default burst = 1s worth of tokens
  tokens_ = burst_;
  last_ = std::chrono::steady_clock::now();
  initialized_ = true;
}

bool RateLimiter::tryAcquire() {
  std::lock_guard<std::mutex> lk(m_);
  if (rate_ <= 0.0)
    return true; // disabled

  const auto now = std::chrono::steady_clock::now();
  if (!initialized_) {
    last_ = now;
    tokens_ = burst_;
    initialized_ = true;
  }

  const double elapsed = std::chrono::duration<double>(now - last_).count();
  last_ = now;
  tokens_ = std::min(burst_, tokens_ + elapsed * rate_);

  if (tokens_ >= 1.0) {
    tokens_ -= 1.0;
    return true;
  }
  return false;
}

void RiskEngine::configure(const RiskConfig &cfg) {
  maxOrderVolume_.store(cfg.maxOrderVolume, std::memory_order_relaxed);
  maxPriceDeviation_.store(cfg.maxPriceDeviation, std::memory_order_relaxed);
  maxNotional_.store(cfg.maxNotional, std::memory_order_relaxed);
  limiter_.configure(cfg.maxOrdersPerSec, cfg.orderBurst);
}

void RiskEngine::halt() { halted_.store(true, std::memory_order_relaxed); }
void RiskEngine::resume() { halted_.store(false, std::memory_order_relaxed); }

void RiskEngine::setRefPrice(const std::string &instrumentId, double price) {
  std::lock_guard<std::mutex> lk(refMutex_);
  refPrices_[instrumentId] = price;
}

double RiskEngine::refPrice(const std::string &instrumentId) const {
  std::lock_guard<std::mutex> lk(refMutex_);
  auto it = refPrices_.find(instrumentId);
  return it != refPrices_.end() ? it->second : 0.0;
}

RiskVerdict RiskEngine::check(double price, double refPrice, int volume) const {
  if (halted_.load(std::memory_order_relaxed))
    return {false, "trading halted (kill-switch)"};

  if (volume <= 0)
    return {false, "order volume must be positive"};

  const int maxVol = maxOrderVolume_.load(std::memory_order_relaxed);
  if (maxVol > 0 && volume > maxVol)
    return {false, "order volume exceeds maxOrderVolume"};

  const double dev = maxPriceDeviation_.load(std::memory_order_relaxed);
  if (dev > 0.0 && refPrice > 0.0) {
    const double diff = std::fabs(price - refPrice) / refPrice;
    if (diff > dev)
      return {false, "order price deviates too far from reference"};
  }

  const double maxNotional = maxNotional_.load(std::memory_order_relaxed);
  if (maxNotional > 0.0 && price > 0.0) {
    // TODO(multiplier): multiply by contract size once instrument info exists.
    const double notional = price * static_cast<double>(volume);
    if (notional > maxNotional)
      return {false, "order notional exceeds maxNotional"};
  }

  return {true, nullptr};
}

} // namespace ctp
