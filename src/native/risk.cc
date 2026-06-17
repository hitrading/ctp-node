/*
 * risk.cc - see risk.h
 */

#include "risk.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace ctp {

// A price / multiplier / fill quantity is usable in the cost & notional math only
// if it is finite, strictly positive, and below the CTP "unset" sentinel band.
// DBL_MAX (~1.8e308, what CTP puts in untouched price fields before the first
// trade) IS finite, so it slips past isfinite alone; 1e300 is far above any real
// value and far below DBL_MAX. Centralized so EVERY double entering the math
// rejects the sentinel uniformly: a split policy is exactly what let DBL_MAX
// poison setMultiplier/onTrade after the reference-price path was already guarded
// (rounds 11/14). A sentinel/Inf multiplier or fill makes cost Inf and a later
// proportional close-release Inf-Inf = NaN, and NaN > cap is false -> the cost
// cap is silently voided. Reject at the door instead.
static inline bool sanePositive(double x) {
  return std::isfinite(x) && x > 0.0 && x < 1e300;
}

// Keep an accumulated open-cost running sum finite. Each fill's cost passes the
// per-fill backstop, but a long chain of huge (test-hook / absurd-seed) fills can
// still overflow the SUM to +Inf, and a later proportional close (Inf - Inf)
// would yield NaN, silently voiding the cost cap (NaN > cap is false). Pin an
// overflowed sum to the largest finite double: it stays above EVERY finite
// configured cap (so it keeps blocking - +Inf would block too but breaks the
// proportional close) while keeping that close arithmetic finite. Unreachable
// from the live feed (real fills are tiny); defense-in-depth matching sanePositive.
static inline void clampFiniteCost(double &cost) {
  if (!std::isfinite(cost))
    cost = std::numeric_limits<double>::max();
}

// Price for RESERVATION cost math. The held-cost path (onTrade) guards its price
// via sanePositive, but the in-flight RESERVATION cost (tryReserveOpen /
// onOrderUpdate / rebuildOpenReservations) is fed limit prices straight from the
// order / CTP. A non-finite or DBL_MAX-sentinel price would reserve an Inf cost,
// and when that order goes terminal the reconcile (Inf - Inf) yields NaN, which
// silently voids EVERY cost cap book-wide (NaN > cap is false). Treat a
// non-usable price as 0 cost: the order's volume is still capped, the cost
// reservation is simply skipped, and no Inf/NaN can enter the ledger. A market
// order's legitimate 0 price already maps to 0 here, unchanged.
static inline double costPrice(double price) {
  return sanePositive(price) ? price : 0.0;
}

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
  // maxMargin is the canonical margin-based global cap; maxPositionCost is the
  // deprecated alias. Prefer maxMargin when set.
  maxPositionCost_.store(cfg.maxMargin > 0.0 ? cfg.maxMargin : cfg.maxPositionCost,
                         std::memory_order_relaxed);
  limiter_.configure(cfg.maxOrdersPerSec, cfg.orderBurst);
}

void RiskEngine::halt() { halted_.store(true, std::memory_order_relaxed); }
void RiskEngine::resume() { halted_.store(false, std::memory_order_relaxed); }

void RiskEngine::setMdSnapshot(std::shared_ptr<const RefPriceSource> source) {
  std::lock_guard<std::mutex> lk(refMutex_);
  mdSnap_ = std::move(source);
}

double RiskEngine::refPrice(const std::string &instrumentId) const {
  // Read the deviation reference from the live MD snapshot (in C++). Copy the
  // shared_ptr under our lock, then query outside it (the cache has its own
  // mutex) — no nested locking, and a concurrent detach can't free it mid-read.
  // A non-finite / DBL_MAX "unset" last price is rejected here (not in the cache)
  // so a sentinel reference can't make |price-ref|/ref == 1.0 and falsely block
  // every order — the deviation check skips on a 0 reference.
  std::shared_ptr<const RefPriceSource> snap;
  {
    std::lock_guard<std::mutex> lk(refMutex_);
    snap = mdSnap_;
  }
  if (!snap)
    return 0.0;
  const double px = snap->last(instrumentId);
  return sanePositive(px) ? px : 0.0;
}

void RiskEngine::setMultiplier(const std::string &instrumentId, double mult) {
  std::lock_guard<std::mutex> lk(posMutex_);
  // A usable (finite, positive, sub-sentinel) multiplier is taken as-is; anything
  // else (<=0, NaN, +Inf, or a DBL_MAX-class value) falls back to 1.0 — see
  // sanePositive for why the sentinel band matters.
  multipliers_[instrumentId] = sanePositive(mult) ? mult : 1.0;
}

void RiskEngine::setMarginRate(const std::string &instrumentId, double rate) {
  std::lock_guard<std::mutex> lk(posMutex_);
  // Store only a usable rate (finite, positive, sub-sentinel). A 0/invalid rate
  // (e.g. an all-by-volume contract reporting 0 by-money, or a malformed field)
  // is ignored so the conservative default keeps applying — zeroing the margin
  // would silently void the cap for that instrument.
  if (sanePositive(rate))
    marginRate_[instrumentId] = rate;
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

double RiskEngine::marginRateLocked(const std::string &instrumentId) const {
  auto it = marginRate_.find(instrumentId);
  return it != marginRate_.end() ? it->second : kDefaultMarginRate;
}

double RiskEngine::costFactorLocked(const std::string &instrumentId) const {
  // The tracked open "cost" is MARGIN, so every cost computation scales the
  // notional factor (multiplier) by the instrument's margin rate.
  return multiplierLocked(instrumentId) * marginRateLocked(instrumentId);
}

void RiskEngine::seedPosition(const std::string &instrumentId, bool isLong,
                              double volume, double cost) {
  // Reject non-finite/negative seeds so a bad value can't poison positionCost
  // (a NaN would silently disable the cost cap, since NaN > cap is false).
  if (!std::isfinite(volume) || !std::isfinite(cost) || volume < 0.0 || cost < 0.0)
    return;
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
  // Real CTP fills are always positive, finite, and far below the sentinel band;
  // ignore malformed data rather than let it corrupt the tracked cost (a NaN
  // would silently void the cost cap, +Inf/DBL_MAX would jam or void it). See
  // sanePositive — same uniform sentinel rejection as setMultiplier / refPrice.
  if (!sanePositive(price) || !sanePositive(volume))
    return;
  std::lock_guard<std::mutex> lk(posMutex_);
  const double cost = price * volume * costFactorLocked(instrumentId);
  // Per-fill backstop: even with sanePositive inputs the PRODUCT could be
  // non-finite (e.g. 1e299 * 1e299); never let a non-finite single-fill cost in.
  if (!std::isfinite(cost))
    return;
  Pos &p = positions_[instrumentId];
  if (isOpen) {
    if (isBuy) {
      p.longVol += volume;
      p.longCost += cost;
      clampFiniteCost(p.longCost);
    } else {
      p.shortVol += volume;
      p.shortCost += cost;
      clampFiniteCost(p.shortCost);
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
  const double addCost = volume * costPrice(price) * costFactorLocked(instrumentId);
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
  // The order still holds live working volume UNLESS it has reached a terminal
  // state. Release only on a definitively-done status: AllTraded '0',
  // PartTradedNotQueueing '2', NoTradeNotQueueing '4', Canceled '5'. Everything
  // else — queueing ('1'/'3') and the transient pre-trade states a conditional
  // order can report before '3' (Unknown 'a', NotTouched 'b', Touched 'c'), plus
  // any status CTP might add later — keeps the reservation. Releasing on a
  // transient status that merely precedes '3' would briefly drop the cap for
  // this order's volume (a burst could then slip past). Conservative by design:
  // hold the reservation until the order is provably finished.
  const bool terminal =
      (status == '0' || status == '2' || status == '4' || status == '5');
  double desiredVol = terminal ? 0.0 : volTotal - volTraded;
  if (desiredVol < 0.0)
    desiredVol = 0.0;
  const double factor = costFactorLocked(instrumentId);
  const double desiredCost = desiredVol * costPrice(limitPrice) * factor;

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
    const std::string key = resvKey(o.frontId, o.sessionId, o.orderRef);
    // Dedupe by key: reqQryOrder can return the same working order more than once
    // (paged / duplicate rows). Each (front:session:ref) must reserve exactly
    // once; a duplicate would inflate the pending counters while reservations_
    // collapses to a single entry, leaving a phantom reservation with no map
    // entry to release it - it would over-block opens until the next full resync.
    if (reservations_.count(key))
      continue;
    const double cost = o.vol * costPrice(o.price) * costFactorLocked(o.instrumentId);
    Pos &pp = positions_[o.instrumentId];
    if (o.isLong) {
      pp.longPendVol += o.vol;
      pp.longPendCost += cost;
    } else {
      pp.shortPendVol += o.vol;
      pp.shortPendCost += cost;
    }
    reservations_[key] = Reservation{o.instrumentId, o.isLong, o.vol, cost};
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

  // Reject a non-finite limit price (NaN/Inf) here, at the validation point, so
  // it can't reach tryReserveOpen and poison the reserved cost. A market/any-
  // price order legitimately has price 0 (finite), so isfinite allows it.
  if (!std::isfinite(price))
    return block("order price is not finite");

  const int maxVol = maxOrderVolume_.load(std::memory_order_relaxed);
  if (maxVol > 0 && volume > maxVol)
    return block("order volume exceeds maxOrderVolume");

  const double dev = maxPriceDeviation_.load(std::memory_order_relaxed);
  // price > 0 guard: a market/any-price order has LimitPrice 0, which must NOT
  // be measured against the reference (|0-ref|/ref = 1.0 would always trip).
  if (dev > 0.0 && refPrice > 0.0 && price > 0.0) {
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
