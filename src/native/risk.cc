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
  maxPositionCost_.store(cfg.maxPositionCost, std::memory_order_relaxed);
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

void RiskEngine::setMultiplier(const std::string &instrumentId, double mult) {
  std::lock_guard<std::mutex> lk(posMutex_);
  multipliers_[instrumentId] = mult > 0.0 ? mult : 1.0;
}

void RiskEngine::setMaxPositionVolume(const std::string &instrumentId,
                                      bool isLong, double maxVolume) {
  std::lock_guard<std::mutex> lk(posMutex_);
  VolCap &c = maxPositionVol_[instrumentId];
  (isLong ? c.longMax : c.shortMax) = maxVolume > 0.0 ? maxVolume : 0.0; // 0=off
}

double RiskEngine::multiplierLocked(const std::string &instrumentId) const {
  auto it = multipliers_.find(instrumentId);
  return it != multipliers_.end() ? it->second : 1.0;
}

void RiskEngine::seedPosition(const std::string &instrumentId, bool isLong,
                              double volume, double cost) {
  std::lock_guard<std::mutex> lk(posMutex_);
  Pos &p = positions_[instrumentId];
  if (isLong) {
    p.longVol = volume;
    p.longCost = cost;
  } else {
    p.shortVol = volume;
    p.shortCost = cost;
  }
}

void RiskEngine::resetPositions() {
  std::lock_guard<std::mutex> lk(posMutex_);
  positions_.clear();
}

void RiskEngine::onTrade(const std::string &instrumentId, bool isBuy,
                         bool isOpen, double price, double volume) {
  std::lock_guard<std::mutex> lk(posMutex_);
  const double cost = price * volume * multiplierLocked(instrumentId);
  Pos &p = positions_[instrumentId];
  if (isOpen) {
    if (isBuy) {
      p.longVol += volume;
      p.longCost += cost;
    } else {
      p.shortVol += volume;
      p.shortCost += cost;
    }
  } else if (isBuy) { // closing a short: release its open cost proportionally
    const double v = std::min(volume, p.shortVol);
    const double rel = p.shortVol > 0.0 ? p.shortCost * (v / p.shortVol) : 0.0;
    p.shortCost -= rel;
    p.shortVol -= v;
  } else { // closing a long
    const double v = std::min(volume, p.longVol);
    const double rel = p.longVol > 0.0 ? p.longCost * (v / p.longVol) : 0.0;
    p.longCost -= rel;
    p.longVol -= v;
  }
}

double RiskEngine::currentPositionCost() const {
  std::lock_guard<std::mutex> lk(posMutex_);
  double total = 0.0;
  for (const auto &kv : positions_)
    total += kv.second.longCost + kv.second.shortCost;
  return total;
}

bool RiskEngine::allowOpen(const std::string &instrumentId, double price,
                           double volume) const {
  const double maxCost = maxPositionCost_.load(std::memory_order_relaxed);
  if (maxCost <= 0.0)
    return true;
  std::lock_guard<std::mutex> lk(posMutex_);
  double total = 0.0;
  for (const auto &kv : positions_)
    total += kv.second.longCost + kv.second.shortCost;
  return total + price * volume * multiplierLocked(instrumentId) <= maxCost;
}

bool RiskEngine::allowOpenVolume(const std::string &instrumentId, bool isLong,
                                 double volume) const {
  std::lock_guard<std::mutex> lk(posMutex_);
  auto it = maxPositionVol_.find(instrumentId);
  if (it == maxPositionVol_.end())
    return true; // no cap for this instrument
  const double cap = isLong ? it->second.longMax : it->second.shortMax;
  if (cap <= 0.0)
    return true; // this side uncapped
  double held = 0.0;
  auto pit = positions_.find(instrumentId);
  if (pit != positions_.end())
    held = isLong ? pit->second.longVol : pit->second.shortVol;
  return held + volume <= cap;
}

RiskVerdict RiskEngine::check(const std::string &instrumentId, double price,
                              double refPrice, int volume) const {
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
    double mult;
    {
      std::lock_guard<std::mutex> lk(posMutex_); // only when notional is capped
      mult = multiplierLocked(instrumentId);
    }
    const double notional = price * static_cast<double>(volume) * mult;
    if (notional > maxNotional)
      return {false, "order notional exceeds maxNotional"};
  }

  return {true, nullptr};
}

} // namespace ctp
