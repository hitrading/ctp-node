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
  positions_.clear(); // clears held AND in-flight pending (both live in Pos)
  reservations_.clear();
}

void RiskEngine::onTrade(const std::string &instrumentId, bool isBuy,
                         bool isOpen, double price, double volume) {
  // Real CTP fills are always positive; ignore malformed data rather than let a
  // negative price/volume corrupt the tracked cost (defense in depth).
  if (price <= 0.0 || volume <= 0.0)
    return;
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

void RiskEngine::setMaxInstrumentCost(const std::string &instrumentId,
                                     double maxCost) {
  std::lock_guard<std::mutex> lk(posMutex_);
  maxInstrumentCost_[instrumentId] = maxCost > 0.0 ? maxCost : 0.0; // 0 = off
}

bool RiskEngine::hasCapLocked(const std::string &instrumentId, bool isLong) const {
  if (maxPositionCost_.load(std::memory_order_relaxed) > 0.0)
    return true;
  auto cit = maxInstrumentCost_.find(instrumentId);
  if (cit != maxInstrumentCost_.end() && cit->second > 0.0)
    return true;
  auto vit = maxPositionVol_.find(instrumentId);
  if (vit != maxPositionVol_.end() &&
      (isLong ? vit->second.longMax : vit->second.shortMax) > 0.0)
    return true;
  return false;
}

void RiskEngine::setSession(int frontId, int sessionId) {
  std::lock_guard<std::mutex> lk(posMutex_);
  myFrontId_ = frontId;
  mySessionId_ = sessionId;
}

std::string RiskEngine::resvKey(int frontId, int sessionId,
                                const std::string &orderRef) {
  return std::to_string(frontId) + ":" + std::to_string(sessionId) + ":" + orderRef;
}

OpenGate RiskEngine::tryReserveOpen(const std::string &orderRef,
                                    const std::string &instrumentId, bool isLong,
                                    double price, double volume) {
  const double globalCap = maxPositionCost_.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(posMutex_);
  const double addCost = price * volume * multiplierLocked(instrumentId);
  const Pos *p = nullptr;
  auto pit = positions_.find(instrumentId);
  if (pit != positions_.end())
    p = &pit->second;
  bool capped = false;

  // 1) per-side volume cap (committed = held + reserved on this side)
  auto vit = maxPositionVol_.find(instrumentId);
  if (vit != maxPositionVol_.end()) {
    const double cap = isLong ? vit->second.longMax : vit->second.shortMax;
    if (cap > 0.0) {
      capped = true;
      double committed = 0.0;
      if (p)
        committed = isLong ? p->longVol + p->longPendVol
                           : p->shortVol + p->shortPendVol;
      if (committed + volume > cap)
        return OpenGate::VolumeLimit;
    }
  }

  // 2) per-instrument cost cap (both sides, held + reserved)
  auto cit = maxInstrumentCost_.find(instrumentId);
  if (cit != maxInstrumentCost_.end() && cit->second > 0.0) {
    capped = true;
    double instrCost = 0.0;
    if (p)
      instrCost = p->longCost + p->longPendCost + p->shortCost + p->shortPendCost;
    if (instrCost + addCost > cit->second)
      return OpenGate::CostLimit;
  }

  // 3) global cost cap (whole book, held + reserved)
  if (globalCap > 0.0) {
    capped = true;
    double total = 0.0;
    for (const auto &kv : positions_)
      total += kv.second.longCost + kv.second.longPendCost +
               kv.second.shortCost + kv.second.shortPendCost;
    if (total + addCost > globalCap)
      return OpenGate::CostLimit;
  }

  // Passed every active cap -> reserve (only if a cap is in force; otherwise
  // there is nothing to enforce and tracking would just be overhead).
  if (capped) {
    Pos &pp = positions_[instrumentId];
    if (isLong) {
      pp.longPendVol += volume;
      pp.longPendCost += addCost;
    } else {
      pp.shortPendVol += volume;
      pp.shortPendCost += addCost;
    }
    reservations_[resvKey(myFrontId_, mySessionId_, orderRef)] =
        Reservation{instrumentId, isLong, volume, addCost};
  }
  return OpenGate::Ok;
}

void RiskEngine::onOrderUpdate(int frontId, int sessionId,
                               const std::string &orderRef,
                               const std::string &instrumentId, bool isOpen,
                               bool isLong, char status, double limitPrice,
                               double volTotal, double volTraded) {
  if (!isOpen)
    return; // only opening orders reserve
  std::lock_guard<std::mutex> lk(posMutex_);
  const std::string key = resvKey(frontId, sessionId, orderRef);
  const bool working = (status == '1' || status == '3'); // queueing
  double desiredVol = working ? volTotal - volTraded : 0.0;
  if (desiredVol < 0.0)
    desiredVol = 0.0;
  const double mult = multiplierLocked(instrumentId);
  const double desiredCost = desiredVol * limitPrice * mult;

  auto rit = reservations_.find(key);
  if (rit == reservations_.end()) {
    // Untracked order (sent before a cap existed, or placed by another terminal
    // on this account): start tracking only if a cap applies and it's working.
    if (desiredVol <= 0.0 || !hasCapLocked(instrumentId, isLong))
      return;
    Pos &pp = positions_[instrumentId];
    if (isLong) {
      pp.longPendVol += desiredVol;
      pp.longPendCost += desiredCost;
    } else {
      pp.shortPendVol += desiredVol;
      pp.shortPendCost += desiredCost;
    }
    reservations_[key] = Reservation{instrumentId, isLong, desiredVol, desiredCost};
    return;
  }

  // Reconcile the existing reservation to the order's current working remainder.
  Reservation &r = rit->second;
  const double dVol = desiredVol - r.vol;
  const double dCost = desiredCost - r.cost;
  Pos &pp = positions_[r.instrumentId];
  if (r.isLong) {
    pp.longPendVol += dVol;
    pp.longPendCost += dCost;
  } else {
    pp.shortPendVol += dVol;
    pp.shortPendCost += dCost;
  }
  if (desiredVol <= 0.0)
    reservations_.erase(rit); // terminal: fully released
  else {
    r.vol = desiredVol;
    r.cost = desiredCost;
  }
}

void RiskEngine::releaseReservation(const std::string &orderRef) {
  std::lock_guard<std::mutex> lk(posMutex_);
  // Only ever called for OUR own orders (failed send / front rejection).
  auto rit = reservations_.find(resvKey(myFrontId_, mySessionId_, orderRef));
  if (rit == reservations_.end())
    return;
  Reservation &r = rit->second;
  Pos &pp = positions_[r.instrumentId];
  if (r.isLong) {
    pp.longPendVol -= r.vol;
    pp.longPendCost -= r.cost;
  } else {
    pp.shortPendVol -= r.vol;
    pp.shortPendCost -= r.cost;
  }
  reservations_.erase(rit);
}

void RiskEngine::rebuildOpenReservations(const std::vector<OpenOrderInfo> &orders) {
  std::lock_guard<std::mutex> lk(posMutex_);
  // Authoritative resync: drop all reservations and the in-flight portion of
  // every position, then re-reserve from the live working orders. Held (filled)
  // position is untouched - that's reconciled by syncPositions.
  for (auto &kv : positions_) {
    kv.second.longPendVol = kv.second.longPendCost = 0.0;
    kv.second.shortPendVol = kv.second.shortPendCost = 0.0;
  }
  reservations_.clear();
  for (const auto &o : orders) {
    if (o.vol <= 0.0)
      continue;
    const double cost = o.vol * o.price * multiplierLocked(o.instrumentId);
    Pos &pp = positions_[o.instrumentId];
    if (o.isLong) {
      pp.longPendVol += o.vol;
      pp.longPendCost += cost;
    } else {
      pp.shortPendVol += o.vol;
      pp.shortPendCost += cost;
    }
    reservations_[resvKey(o.frontId, o.sessionId, o.orderRef)] =
        Reservation{o.instrumentId, o.isLong, o.vol, cost};
  }
}

RiskVerdict RiskEngine::block(const char *reason) const {
  lastReason_.store(reason, std::memory_order_relaxed);
  return {false, reason};
}

RiskVerdict RiskEngine::check(const std::string &instrumentId, double price,
                              double refPrice, int volume) const {
  if (halted_.load(std::memory_order_relaxed))
    return block("trading halted (kill-switch)");

  if (volume <= 0)
    return block("order volume must be positive");

  const int maxVol = maxOrderVolume_.load(std::memory_order_relaxed);
  if (maxVol > 0 && volume > maxVol)
    return block("order volume exceeds maxOrderVolume");

  const double dev = maxPriceDeviation_.load(std::memory_order_relaxed);
  if (dev > 0.0 && refPrice > 0.0) {
    const double diff = std::fabs(price - refPrice) / refPrice;
    if (diff > dev)
      return block("order price deviates too far from reference");
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
      return block("order notional exceeds maxNotional");
  }

  return {true, nullptr};
}

} // namespace ctp
