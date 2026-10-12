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
// poison instrument metadata/onTrade after the reference-price path was guarded
// (rounds 11/14). A sentinel/Inf multiplier or fill makes cost Inf and a later
// proportional close-release Inf-Inf = NaN, and NaN > cap is false -> the cost
// cap is silently voided. Reject at the door instead.
static inline bool sanePositive(double x) {
  return std::isfinite(x) && x > 0.0 && x < 1e300;
}

static inline bool saneLots(double x) {
  return std::isfinite(x) && x > 0.0 && x <= 2147483647.0 &&
         std::floor(x) == x;
}

static inline bool saneNonNegativeLots(double x) {
  return x == 0.0 || saneLots(x);
}

static inline bool validHedgeFlag(char flag) {
  return flag == '1' || flag == '2' || flag == '3' || flag == '5' ||
         flag == '6' || flag == '7';
}

static inline bool terminalOrderStatus(char status) {
  return status == '0' || status == '2' || status == '4' || status == '5';
}

static inline bool validTradingDay(const std::string &day) {
  if (day.size() != 8)
    return false;
  for (char c : day)
    if (c < '0' || c > '9')
      return false;
  const int year = std::stoi(day.substr(0, 4));
  const int month = std::stoi(day.substr(4, 2));
  const int date = std::stoi(day.substr(6, 2));
  if (year == 0 || month < 1 || month > 12)
    return false;
  static constexpr int daysByMonth[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int maxDate = daysByMonth[month];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap)
    ++maxDate;
  return date >= 1 && date <= maxDate;
}

static inline double positiveLimitOrDisabled(double x) {
  // Exactly zero disables a limit. Negative/NaN/Inf is never a valid limit:
  // make it fail closed for native callers that bypass the TS guards.
  if (!std::isfinite(x) || x < 0.0)
    return std::numeric_limits<double>::min();
  return x > 0.0 ? x : 0.0;
}

// Keep an accumulated open-margin running sum finite. Each fill's margin passes the
// per-fill backstop, but a long chain of huge (test-hook / absurd-seed) fills can
// still overflow the SUM to +Inf, and a later proportional close (Inf - Inf)
// would yield NaN, silently voiding the margin cap (NaN > cap is false). Pin an
// overflowed sum to the largest finite double: it stays above EVERY finite
// configured cap (so it keeps blocking - +Inf would block too but breaks the
// proportional close) while keeping that close arithmetic finite. Unreachable
// from the live feed (real fills are tiny); defense-in-depth matching sanePositive.
static inline void clampFiniteCost(double &cost) {
  if (!std::isfinite(cost))
    cost = std::numeric_limits<double>::max();
}

void RateLimiter::configure(double ratePerSec, double burst) {
  std::lock_guard<std::mutex> lk(m_);
  if (!std::isfinite(ratePerSec) || !std::isfinite(burst) ||
      ratePerSec < 0.0 || burst < 0.0) {
    rate_ = std::numeric_limits<double>::min();
    burst_ = 1.0;
    tokens_ = 0.0; // malformed native config fails closed
    last_ = std::chrono::steady_clock::now();
    initialized_ = true;
    return;
  }
  rate_ = positiveLimitOrDisabled(ratePerSec);
  burst_ = positiveLimitOrDisabled(burst);
  if (burst_ <= 0.0)
    burst_ = rate_; // default burst = 1s worth of tokens
  if (rate_ > 0.0)
    burst_ = std::max(1.0, burst_); // a bucket below one token can never fire
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

bool RateLimiter::enabled() const {
  std::lock_guard<std::mutex> lk(m_);
  return rate_ > 0.0;
}

void RiskEngine::configure(const RiskLimits &cfg) {
  std::lock_guard<RiskSendMutex> sendBarrier(sendModeMutex_);
  const bool invalid =
      cfg.maxOrderVolume < 0 || !std::isfinite(cfg.maxPriceDeviation) ||
      cfg.maxPriceDeviation < 0.0 || !std::isfinite(cfg.maxNotional) ||
      cfg.maxNotional < 0.0 || !std::isfinite(cfg.maxOrdersPerSec) ||
      cfg.maxOrdersPerSec < 0.0 || !std::isfinite(cfg.orderBurst) ||
      cfg.orderBurst < 0.0 || !std::isfinite(cfg.maxCloseOrdersPerSec) ||
      cfg.maxCloseOrdersPerSec < 0.0 || !std::isfinite(cfg.closeOrderBurst) ||
      cfg.closeOrderBurst < 0.0 || !std::isfinite(cfg.maxAccountMargin) ||
      cfg.maxAccountMargin < 0.0 || !std::isfinite(cfg.maxReferenceAgeMs) ||
      cfg.maxReferenceAgeMs <= 0.0;
  invalidConfig_.store(invalid, std::memory_order_release);
  maxOrderVolume_.store(cfg.maxOrderVolume > 0 ? cfg.maxOrderVolume : 0,
                        std::memory_order_relaxed);
  maxPriceDeviation_.store(positiveLimitOrDisabled(cfg.maxPriceDeviation),
                           std::memory_order_relaxed);
  maxNotional_.store(positiveLimitOrDisabled(cfg.maxNotional),
                     std::memory_order_relaxed);
  maxAccountMargin_.store(positiveLimitOrDisabled(cfg.maxAccountMargin),
                          std::memory_order_relaxed);
  maxReferenceAgeMs_.store(invalid ? 0.0 : cfg.maxReferenceAgeMs,
                           std::memory_order_relaxed);
  limiter_.configure(cfg.maxOrdersPerSec, cfg.orderBurst);
  closeLimiter_.configure(cfg.maxCloseOrdersPerSec, cfg.closeOrderBurst);
}

void RiskEngine::reduceOnly() {
  std::lock_guard<RiskSendMutex> lk(sendModeMutex_);
  mode_.store(RiskMode::ReduceOnly, std::memory_order_release);
}
void RiskEngine::haltAll() {
  std::lock_guard<RiskSendMutex> lk(sendModeMutex_);
  mode_.store(RiskMode::HaltAll, std::memory_order_release);
}
void RiskEngine::resume() {
  std::lock_guard<RiskSendMutex> lk(sendModeMutex_);
  mode_.store(RiskMode::Normal, std::memory_order_release);
}

std::unique_lock<RiskSendMutex> RiskEngine::acquireOrderSend(bool isOpen,
                                                             bool &allowed) {
  std::unique_lock<RiskSendMutex> lock(sendModeMutex_);
  const RiskMode current = mode_.load(std::memory_order_acquire);
  allowed = current != RiskMode::HaltAll &&
            !(current == RiskMode::ReduceOnly && isOpen);
  if (!allowed)
    lastReason_ = current == RiskMode::HaltAll
                      ? "all order inserts halted"
                      : "opening orders blocked in reduce-only mode";
  return lock;
}

std::unique_lock<RiskSendMutex> RiskEngine::acquireOrderSendForAccount(
    bool isOpen, const std::string &brokerId,
    const std::string &investorId, bool &allowed) {
  std::unique_lock<RiskSendMutex> lock(sendModeMutex_);
  const RiskMode current = mode_.load(std::memory_order_acquire);
  allowed = current != RiskMode::HaltAll &&
            !(current == RiskMode::ReduceOnly && isOpen);
  if (allowed) {
    std::lock_guard<std::mutex> account(posMutex_);
    allowed = accountScopeLocked(brokerId, investorId) == AccountScope::Current;
    if (!allowed)
      lastReason_ = "order account does not match confirmed session";
  } else {
    lastReason_ = current == RiskMode::HaltAll
                      ? "all order inserts halted"
                      : "opening orders blocked in reduce-only mode";
  }
  return lock;
}

void RiskEngine::setMdSnapshot(std::shared_ptr<const RefPriceSource> source) {
  std::lock_guard<std::mutex> lk(refMutex_);
  mdSnap_ = std::move(source);
}

double RiskEngine::refPrice(const std::string &instrumentId) const {
  // Read the deviation reference from the live MD snapshot (in C++). Copy the
  // shared_ptr under our lock, then query outside it (the cache has its own
  // mutex); no nested locking, and a concurrent detach can't free it mid-read.
  // A non-finite / DBL_MAX "unset" last price is rejected here (not in the cache)
  // so a sentinel reference can't make |price-ref|/ref == 1.0 and falsely block
  // every order. An enabled deviation guard fails closed on a 0 reference.
  std::shared_ptr<const RefPriceSource> snap;
  {
    std::lock_guard<std::mutex> lk(refMutex_);
    snap = mdSnap_;
  }
  if (!snap)
    return 0.0;
  double px = 0.0;
  uint64_t age = 0;
  if (!snap->quote(instrumentId, px, age))
    return 0.0;
  if (!sanePositive(px))
    return 0.0;
  const double maxAge = maxReferenceAgeMs_.load(std::memory_order_relaxed);
  if (maxAge >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
    return px;
  return age <= static_cast<uint64_t>(maxAge) ? px : 0.0;
}

void RiskEngine::setInstrument(const std::string &instrumentId, double mult,
                               char productClass, const std::string &productId) {
  if (instrumentId.empty())
    return;
  // Metadata changes can increase the computed exposure of a just-reserved
  // order. Serialize the refresh with the same final barrier as cap changes so
  // no order is sent using a half-old margin model.
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (!sanePositive(mult)) {
    multipliers_.erase(instrumentId);
    futuresMarginFormula_.erase(instrumentId);
    instrumentProducts_.erase(instrumentId);
    invalidateHeldCostsLocked(instrumentId);
    refreshReservationCostsLocked(instrumentId);
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  multipliers_[instrumentId] = mult;
  if (productId.empty()) instrumentProducts_.erase(instrumentId);
  else instrumentProducts_[instrumentId] = productId;
  // Futures and TAS use the futures margin schedule represented here. Options,
  // combinations and spot products require product-specific fields/formulas.
  futuresMarginFormula_[instrumentId] =
      productClass == '1' || productClass == '7';
  invalidateHeldCostsLocked(instrumentId);
  refreshReservationCostsLocked(instrumentId);
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::invalidateInstrument(const std::string &instrumentId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (instrumentId.empty()) {
    multipliers_.clear();
    futuresMarginFormula_.clear();
    instrumentProducts_.clear();
    invalidateHeldCostsLocked("");
    refreshAllReservationCostsLocked();
  } else {
    multipliers_.erase(instrumentId);
    futuresMarginFormula_.erase(instrumentId);
    instrumentProducts_.erase(instrumentId);
    invalidateHeldCostsLocked(instrumentId);
    refreshReservationCostsLocked(instrumentId);
  }
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::setMarginScheduleLocked(
    const std::string &instrumentId, char hedgeFlag, double longByMoney,
    double longByVolume, double shortByMoney, double shortByVolume,
    bool isRelative) {
  auto valid = [](double x) {
    return std::isfinite(x) && x >= 0.0 && x < 1e300;
  };
  if (instrumentId.empty() || !validHedgeFlag(hedgeFlag))
    return;
  const std::string key = marginKey(instrumentId, hedgeFlag);
  const bool usable = valid(longByMoney) && valid(longByVolume) &&
                      valid(shortByMoney) && valid(shortByVolume) &&
                      (isRelative ||
                       ((longByMoney > 0.0 || longByVolume > 0.0) &&
                        (shortByMoney > 0.0 || shortByVolume > 0.0)));
  if (usable)
    marginRates_[key] = MarginSchedule{longByMoney, longByVolume, shortByMoney,
                                       shortByVolume, isRelative};
  else
    marginRates_.erase(key);
  invalidateHeldCostsLocked(instrumentId);
  refreshReservationCostsLocked(instrumentId);
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::setMarginSchedule(const std::string &instrumentId,
                                   char hedgeFlag, double longByMoney,
                                   double longByVolume, double shortByMoney,
                                   double shortByVolume, bool isRelative) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  setMarginScheduleLocked(instrumentId, hedgeFlag, longByMoney, longByVolume,
                          shortByMoney, shortByVolume, isRelative);
}

void RiskEngine::setExchangeMarginScheduleLocked(
    const std::string &instrumentId, char hedgeFlag, double longByMoney,
    double longByVolume, double shortByMoney, double shortByVolume) {
  auto valid = [](double x) {
    return std::isfinite(x) && x >= 0.0 && x < 1e300;
  };
  if (instrumentId.empty() || !validHedgeFlag(hedgeFlag))
    return;
  const std::string key = marginKey(instrumentId, hedgeFlag);
  const bool usable = valid(longByMoney) && valid(longByVolume) &&
                      valid(shortByMoney) && valid(shortByVolume) &&
                      (longByMoney > 0.0 || longByVolume > 0.0) &&
                      (shortByMoney > 0.0 || shortByVolume > 0.0);
  if (usable)
    exchangeMarginRates_[key] = MarginSchedule{
        longByMoney, longByVolume, shortByMoney, shortByVolume, false};
  else
    exchangeMarginRates_.erase(key);
  invalidateHeldCostsLocked(instrumentId);
  refreshReservationCostsLocked(instrumentId);
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::setExchangeMarginSchedule(
    const std::string &instrumentId, char hedgeFlag, double longByMoney,
    double longByVolume, double shortByMoney, double shortByVolume) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  setExchangeMarginScheduleLocked(instrumentId, hedgeFlag, longByMoney,
                                  longByVolume, shortByMoney, shortByVolume);
}

void RiskEngine::invalidateMarginSchedules(const std::string &instrumentId,
                                           char hedgeFlag) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (instrumentId.empty()) {
    marginRates_.clear();
    exchangeMarginRates_.clear();
    invalidateHeldCostsLocked("");
    refreshAllReservationCostsLocked();
  } else {
    const std::string key = marginKey(instrumentId, hedgeFlag);
    marginRates_.erase(key);
    exchangeMarginRates_.erase(key);
    invalidateHeldCostsLocked(instrumentId);
    refreshReservationCostsLocked(instrumentId);
  }
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::invalidateExchangeMarginSchedule(
    const std::string &instrumentId, char hedgeFlag) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (instrumentId.empty()) {
    exchangeMarginRates_.clear();
    invalidateHeldCostsLocked("");
    refreshAllReservationCostsLocked();
  } else {
    exchangeMarginRates_.erase(marginKey(instrumentId, hedgeFlag));
    invalidateHeldCostsLocked(instrumentId);
    refreshReservationCostsLocked(instrumentId);
  }
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::beginMarginScheduleQuery(
    int requestId, bool exchange, const std::string &instrumentId,
    char hedgeFlag, const std::string &brokerId,
    const std::string &investorId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  activeMarginQueryRequestId_ = requestId > 0 ? requestId : 0;
  activeMarginQueryKind_ = requestId > 0
                               ? (exchange ? MarginQueryKind::Exchange
                                           : MarginQueryKind::Investor)
                               : MarginQueryKind::None;
  activeMarginQueryInstrument_ = instrumentId;
  activeMarginQueryHedgeFlag_ = hedgeFlag;
  stagedMarginRates_.clear();
  activeMarginQueryMembers_.clear();
  invalidStagedMarginKeys_.clear();
  if (!instrumentId.empty() && !multipliers_.count(instrumentId)) {
    for (const auto &entry : instrumentProducts_)
      if (entry.second == instrumentId) activeMarginQueryMembers_.insert(entry.first);
  }
  const bool brokerMatches = brokerId.empty() ||
                             (!myBrokerId_.empty() && brokerId == myBrokerId_);
  const bool investorMatches = exchange || investorId.empty() ||
                               (!myInvestorId_.empty() &&
                                investorId == myInvestorId_);
  activeMarginQueryAuthorized_ = requestId > 0 && sessionIdentityKnown_ &&
                                 brokerMatches && investorMatches &&
                                 (hedgeFlag == '\0' ||
                                  validHedgeFlag(hedgeFlag));
  if (!activeMarginQueryAuthorized_)
    return;
  auto eraseInstrument = [&](auto &rates) {
    if (instrumentId.empty()) {
      rates.clear();
      return;
    }
    for (auto it = rates.begin(); it != rates.end();) {
      const auto split = it->first.rfind('|');
      const auto id = it->first.substr(0, split);
      if ((id == instrumentId || activeMarginQueryMembers_.count(id)) &&
          (hedgeFlag == '\0' || it->first == marginKey(id, hedgeFlag)))
        it = rates.erase(it);
      else
        ++it;
    }
  };
  if (instrumentId.empty()) {
    if (exchange) {
      exchangeMarginRates_.clear();
    } else {
      marginRates_.clear();
      exchangeMarginRates_.clear();
    }
    invalidateHeldCostsLocked("");
    refreshAllReservationCostsLocked();
  } else {
    if (exchange) {
      eraseInstrument(exchangeMarginRates_);
    } else {
      eraseInstrument(marginRates_);
      // A new relative investor row may not combine with an exchange base from
      // an older refresh generation.
      eraseInstrument(exchangeMarginRates_);
    }
    invalidateHeldCostsLocked(instrumentId);
    refreshReservationCostsLocked(instrumentId);
    for (const auto &member : activeMarginQueryMembers_) {
      invalidateHeldCostsLocked(member);
      refreshReservationCostsLocked(member);
    }
  }
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::applyInvestorMarginSchedule(
    int requestId, const std::string &brokerId,
    const std::string &investorId, const std::string &investUnitId,
    const std::string &instrumentId, char hedgeFlag, double longByMoney,
    double longByVolume, double shortByMoney, double shortByVolume,
    bool isRelative) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (requestId <= 0 || requestId != activeMarginQueryRequestId_ ||
      activeMarginQueryKind_ != MarginQueryKind::Investor ||
      !activeMarginQueryAuthorized_ || !sessionIdentityKnown_ ||
      accountScopeLocked(brokerId, investorId) != AccountScope::Current ||
      !investUnitId.empty() ||
      (!activeMarginQueryInstrument_.empty() &&
       instrumentId != activeMarginQueryInstrument_ &&
       !activeMarginQueryMembers_.count(instrumentId)) ||
      (activeMarginQueryHedgeFlag_ != '\0' &&
       hedgeFlag != activeMarginQueryHedgeFlag_))
    return;
  const auto valid = [](double value) {
    return std::isfinite(value) && value >= 0.0 && value < 1e300;
  };
  const bool usable = validHedgeFlag(hedgeFlag) && !instrumentId.empty() &&
                      valid(longByMoney) && valid(longByVolume) &&
                      valid(shortByMoney) && valid(shortByVolume) &&
                      (isRelative ||
                       ((longByMoney > 0.0 || longByVolume > 0.0) &&
                        (shortByMoney > 0.0 || shortByVolume > 0.0)));
  const std::string key = marginKey(instrumentId, hedgeFlag);
  if (usable) {
    invalidStagedMarginKeys_.erase(key);
    stagedMarginRates_[key] = MarginSchedule{
        longByMoney, longByVolume, shortByMoney, shortByVolume, isRelative};
  } else {
    invalidStagedMarginKeys_.insert(key);
    stagedMarginRates_.erase(key);
  }
}

void RiskEngine::applyExchangeMarginSchedule(
    int requestId, const std::string &brokerId,
    const std::string &instrumentId, char hedgeFlag, double longByMoney,
    double longByVolume, double shortByMoney, double shortByVolume) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (requestId <= 0 || requestId != activeMarginQueryRequestId_ ||
      activeMarginQueryKind_ != MarginQueryKind::Exchange ||
      !activeMarginQueryAuthorized_ || !sessionIdentityKnown_ ||
      myBrokerId_.empty() || brokerId != myBrokerId_ ||
      (!activeMarginQueryInstrument_.empty() &&
       instrumentId != activeMarginQueryInstrument_ &&
       !activeMarginQueryMembers_.count(instrumentId)) ||
      (activeMarginQueryHedgeFlag_ != '\0' &&
       hedgeFlag != activeMarginQueryHedgeFlag_))
    return;
  const auto valid = [](double value) {
    return std::isfinite(value) && value >= 0.0 && value < 1e300;
  };
  const bool usable = validHedgeFlag(hedgeFlag) && !instrumentId.empty() &&
                      valid(longByMoney) && valid(longByVolume) &&
                      valid(shortByMoney) && valid(shortByVolume) &&
                      (longByMoney > 0.0 || longByVolume > 0.0) &&
                      (shortByMoney > 0.0 || shortByVolume > 0.0);
  const std::string key = marginKey(instrumentId, hedgeFlag);
  if (usable) {
    invalidStagedMarginKeys_.erase(key);
    stagedMarginRates_[key] = MarginSchedule{
        longByMoney, longByVolume, shortByMoney, shortByVolume, false};
  } else {
    invalidStagedMarginKeys_.insert(key);
    stagedMarginRates_.erase(key);
  }
}

void RiskEngine::finishMarginScheduleQuery(int requestId, bool success) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (requestId <= 0 || requestId != activeMarginQueryRequestId_)
    return;
  if (success && activeMarginQueryAuthorized_) {
    auto &target = activeMarginQueryKind_ == MarginQueryKind::Exchange
                       ? exchangeMarginRates_
                       : marginRates_;
    for (const auto &entry : stagedMarginRates_)
      target[entry.first] = entry.second;
    // Expand only after the complete, successful response. A malformed explicit
    // contract override must never inherit the product schedule.
    for (const auto &member : activeMarginQueryMembers_) {
      for (const char hedge : {'1', '2', '3', '5', '6', '7'}) {
        const auto key = marginKey(member, hedge);
        if (invalidStagedMarginKeys_.count(key)) continue;
        auto row = stagedMarginRates_.find(key);
        if (row == stagedMarginRates_.end())
          row = stagedMarginRates_.find(marginKey(activeMarginQueryInstrument_, hedge));
        if (row != stagedMarginRates_.end()) target[key] = row->second;
      }
      invalidateHeldCostsLocked(member);
      refreshReservationCostsLocked(member);
    }
    invalidateHeldCostsLocked(activeMarginQueryInstrument_);
    if (activeMarginQueryInstrument_.empty())
      refreshAllReservationCostsLocked();
    else
      refreshReservationCostsLocked(activeMarginQueryInstrument_);
    revision_.fetch_add(1, std::memory_order_release);
  }
  activeMarginQueryRequestId_ = 0;
  activeMarginQueryKind_ = MarginQueryKind::None;
  activeMarginQueryAuthorized_ = false;
  activeMarginQueryInstrument_.clear();
  activeMarginQueryHedgeFlag_ = '\0';
  stagedMarginRates_.clear();
  activeMarginQueryMembers_.clear();
  invalidStagedMarginKeys_.clear();
}

bool RiskEngine::setInstrumentPositionLimits(
    const std::vector<InstrumentPositionLimitUpdate> &updates) {
  for (const auto &u : updates) {
    const auto validLimit = [](double value) {
      return std::isfinite(value) && value >= 0.0 &&
             (value == 0.0 || (std::floor(value) == value &&
                               value <= 2147483647.0));
    };
    if (u.instrumentId.empty() || (!u.updateLong && !u.updateShort) ||
        (u.updateLong && !validLimit(u.longLimit)) ||
        (u.updateShort && !validLimit(u.shortLimit)))
      return false;
  }

  // A tightening call must not return while an order that violates a new cap
  // is between its last local check and the vendor API call. The whole batch is
  // committed under the same barrier, so callers never expose a partial config.
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  for (const auto &u : updates) {
    VolCap &cap = maxPositionVol_[u.instrumentId];
    if (u.updateLong)
      cap.longMax = positiveLimitOrDisabled(u.longLimit);
    if (u.updateShort)
      cap.shortMax = positiveLimitOrDisabled(u.shortLimit);
    if (cap.longMax <= 0.0 && cap.shortMax <= 0.0)
      maxPositionVol_.erase(u.instrumentId);
  }
  return true;
}

double RiskEngine::multiplierLocked(const std::string &instrumentId) const {
  auto it = multipliers_.find(instrumentId);
  return it != multipliers_.end() ? it->second : 1.0;
}

bool RiskEngine::multiplierKnownLocked(const std::string &instrumentId) const {
  return multipliers_.find(instrumentId) != multipliers_.end();
}

std::string RiskEngine::marginKey(const std::string &instrumentId,
                                  char hedgeFlag) {
  return instrumentId + "|" + std::string(1, hedgeFlag ? hedgeFlag : '1');
}

bool RiskEngine::marginCostLocked(const std::string &instrumentId, bool isLong,
                                  char hedgeFlag, double price, double volume,
                                  double &cost) const {
  if (!sanePositive(price) || !sanePositive(volume) ||
      !validHedgeFlag(hedgeFlag) || !multiplierKnownLocked(instrumentId))
    return false;
  auto supported = futuresMarginFormula_.find(instrumentId);
  if (supported == futuresMarginFormula_.end() || !supported->second)
    return false;
  auto it = marginRates_.find(marginKey(instrumentId, hedgeFlag));
  if (it == marginRates_.end())
    return false;
  const MarginSchedule &m = it->second;
  double money = isLong ? m.longByMoney : m.shortByMoney;
  double byVolume = isLong ? m.longByVolume : m.shortByVolume;
  if (m.isRelative) {
    auto base = exchangeMarginRates_.find(marginKey(instrumentId, hedgeFlag));
    if (base == exchangeMarginRates_.end())
      return false;
    money += isLong ? base->second.longByMoney : base->second.shortByMoney;
    byVolume += isLong ? base->second.longByVolume
                       : base->second.shortByVolume;
  }
  cost = price * volume * multiplierLocked(instrumentId) * money +
         volume * byVolume;
  return std::isfinite(cost) && cost >= 0.0;
}

bool RiskEngine::hasCostCapLocked(const std::string &instrumentId) const {
  if (maxAccountMargin_.load(std::memory_order_relaxed) > 0.0)
    return true;
  auto it = maxInstrumentMargin_.find(instrumentId);
  return it != maxInstrumentMargin_.end() && it->second > 0.0;
}

bool RiskEngine::localControlsEnabledLocked() const {
  if (invalidConfig_.load(std::memory_order_acquire) ||
      maxOrderVolume_.load(std::memory_order_relaxed) > 0 ||
      maxPriceDeviation_.load(std::memory_order_relaxed) > 0.0 ||
      maxNotional_.load(std::memory_order_relaxed) > 0.0 ||
      maxAccountMargin_.load(std::memory_order_relaxed) > 0.0 ||
      limiter_.enabled() || closeLimiter_.enabled())
    return true;
  for (const auto &kv : maxInstrumentMargin_)
    if (kv.second > 0.0)
      return true;
  for (const auto &kv : maxPositionVol_)
    if (kv.second.longMax > 0.0 || kv.second.shortMax > 0.0)
      return true;
  return false;
}

void RiskEngine::refreshReservationCostsLocked(
    const std::string &instrumentId) {
  for (auto &kv : reservations_) {
    Reservation &r = kv.second;
    if (r.instrumentId != instrumentId || r.vol <= 0.0)
      continue;
    updateReservationExposureLocked(r, r.vol, r.price);
  }
}

void RiskEngine::refreshAllReservationCostsLocked() {
  for (auto &kv : reservations_) {
    Reservation &reservation = kv.second;
    if (reservation.vol > 0.0)
      updateReservationExposureLocked(reservation, reservation.vol,
                                      reservation.price);
  }
}

void RiskEngine::invalidateHeldCostsLocked(
    const std::string &instrumentId) {
  auto mark = [](Pos &position) {
    for (auto &bucket : position.longHeld)
      if (bucket.second.vol > 0.0)
        bucket.second.costKnown = false;
    for (auto &bucket : position.shortHeld)
      if (bucket.second.vol > 0.0)
        bucket.second.costKnown = false;
    if (position.longVol > 0.0)
      position.longCostKnown = false;
    if (position.shortVol > 0.0)
      position.shortCostKnown = false;
  };
  if (instrumentId.empty()) {
    for (auto &entry : positions_)
      mark(entry.second);
    return;
  }
  auto position = positions_.find(instrumentId);
  if (position != positions_.end())
    mark(position->second);
}

void RiskEngine::recomputeHeldSideLocked(Pos &position, bool isLong) {
  auto &held = isLong ? position.longHeld : position.shortHeld;
  double volume = 0.0;
  double cost = 0.0;
  bool known = true;
  for (auto it = held.begin(); it != held.end();) {
    Pos::HeldBucket &bucket = it->second;
    if (bucket.vol <= 0.0) {
      it = held.erase(it);
      continue;
    }
    volume += bucket.vol;
    cost += bucket.cost;
    clampFiniteCost(volume);
    clampFiniteCost(cost);
    known = known && bucket.costKnown;
    ++it;
  }
  if (isLong) {
    position.longVol = volume;
    position.longCost = cost;
    position.longCostKnown = known;
  } else {
    position.shortVol = volume;
    position.shortCost = cost;
    position.shortCostKnown = known;
  }
}

static inline void replaceCostContribution(double &total, double oldCost,
                                           double newCost) {
  // Once a defensive overflow pins a total at max, subtraction is ambiguous;
  // keep it pinned (fail closed) until the next authoritative rebuild.
  if (total == std::numeric_limits<double>::max() && oldCost > 0.0)
    return;
  total = std::max(0.0, total - oldCost);
  total += newCost;
  clampFiniteCost(total);
}

void RiskEngine::updateReservationExposureLocked(Reservation &r,
                                                  double desiredVol,
                                                  double price) {
  desiredVol = std::max(0.0, desiredVol);
  double desiredCost = 0.0;
  bool desiredKnown = true;
  // A release (desiredVol == 0) needs no reference; only a live open is valued,
  // so avoid the refPrice()/marginCost lookup on the frequent fill/cancel/reject
  // release path. Margin at a caller-supplied sell limit can be arbitrarily below
  // the price at which it immediately executes, so value every live open at the
  // greater of its order price and a fresh market reference. Missing/stale
  // references make the reservation unknown, so an enabled monetary gate fails
  // closed.
  if (desiredVol > 0.0) {
    const double reference = refPrice(r.instrumentId);
    const bool priceKnown = sanePositive(price) && sanePositive(reference);
    const double valuationPrice = priceKnown ? std::max(price, reference) : 0.0;
    desiredKnown = priceKnown &&
        marginCostLocked(r.instrumentId, r.isLong, r.hedgeFlag, valuationPrice,
                         desiredVol, desiredCost);
  }
  Pos &p = positions_[r.instrumentId];
  double &pendingVol = r.isLong ? p.longPendVol : p.shortPendVol;
  double &pendingCost = r.isLong ? p.longPendCost : p.shortPendCost;
  size_t &unknown = r.isLong ? p.longPendUnknown : p.shortPendUnknown;
  pendingVol = std::max(0.0, pendingVol - r.vol);
  pendingVol += desiredVol;
  clampFiniteCost(pendingVol);
  replaceCostContribution(pendingCost, r.cost,
                          desiredKnown ? desiredCost : 0.0);
  if (!r.costKnown && desiredKnown && unknown > 0)
    --unknown;
  else if (r.costKnown && !desiredKnown)
    ++unknown;
  r.vol = desiredVol;
  r.cost = desiredKnown ? desiredCost : 0.0;
  r.price = price;
  r.costKnown = desiredKnown;
}

void RiskEngine::indexReservationLocked(const std::string &key) {
  auto it = reservations_.find(key);
  if (it == reservations_.end())
    return;
  reservationKeysByRef_[it->second.orderRef].insert(key);
  if (!it->second.orderSysKey.empty())
    reservationKeyBySys_[it->second.orderSysKey] = key;
}

void RiskEngine::unindexReservationLocked(const std::string &key,
                                          const Reservation &r) {
  auto refs = reservationKeysByRef_.find(r.orderRef);
  if (refs != reservationKeysByRef_.end()) {
    refs->second.erase(key);
    if (refs->second.empty())
      reservationKeysByRef_.erase(refs);
  }
  if (!r.orderSysKey.empty()) {
    auto sys = reservationKeyBySys_.find(r.orderSysKey);
    if (sys != reservationKeyBySys_.end() && sys->second == key)
      reservationKeyBySys_.erase(sys);
  }
}

bool RiskEngine::observeInsertRequestLocked(
    const std::string &key, const Reservation &reservation) {
  if (reservation.insertRequestId == 0 ||
      reservation.insertBindingToken == 0)
    return false; // snapshot/external-terminal order, not one of our inserts
  auto binding = insertRequests_.find(reservation.insertRequestId);
  if (binding == insertRequests_.end() ||
      binding->second.reservationKey != key ||
      binding->second.orderRef != reservation.orderRef ||
      binding->second.token != reservation.insertBindingToken) {
    // Internal ownership metadata must be one-to-one. Retain the reservation
    // and require an authoritative snapshot rather than make a release unsafe.
    orderLedgerUncertain_ = true;
    return true;
  }
  if (binding->second.state == InsertRequestState::Pending)
    binding->second.state = InsertRequestState::Observed;
  return false;
}

std::string RiskEngine::reservationKeyForTradeLocked(
    const std::string &orderSysKey) const {
  if (!orderSysKey.empty()) {
    auto sys = reservationKeyBySys_.find(orderSysKey);
    if (sys != reservationKeyBySys_.end())
      return sys->second;
    // A concrete but unknown exchange order id identifies some other order.
    // Falling back to OrderRef here can collide across front/session and release
    // this terminal's unrelated same-ref reservation.
    return {};
  }
  // OrderRef is only unique inside a front/session, neither of which exists on
  // Trade. Without an exchange id, even one currently-live matching ref can
  // belong to another terminal; never release by ref alone.
  return {};
}

void RiskEngine::resetPositions() {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  // Public "reset positions" resets HELD state only. Working-order reservations
  // are a separate source of committed exposure and must survive a position
  // resync; clearing them created a fail-open window until syncOrders completed.
  for (auto it = positions_.begin(); it != positions_.end();) {
    Pos &p = it->second;
    p.longVol = p.longCost = p.shortVol = p.shortCost = 0.0;
    p.longCostKnown = p.shortCostKnown = true;
    p.longHeld.clear();
    p.shortHeld.clear();
    if (p.longPendVol == 0.0 && p.shortPendVol == 0.0)
      it = positions_.erase(it);
    else
      ++it;
  }
  // A reset is not an authoritative flat-account snapshot. Keep all opening
  // inserts fail-closed until seedFromPositions()/syncPositions() supplies one;
  // otherwise an armed order can fire in the clear-then-seed window.
  ledgerUncertain_ = true;
  revision_.fetch_add(1, std::memory_order_release);
}

bool RiskEngine::replacePositionsIfRevision(
    uint64_t expected, const std::vector<PositionSeed> &seeds) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (revision_.load(std::memory_order_relaxed) != expected)
    return false;
  // Reject the whole snapshot before touching live state. Silently skipping a
  // malformed row after clearing held positions would turn bad input into an
  // authoritative flat book and fail open.
  for (const auto &s : seeds)
    if (s.instrumentId.empty() || !saneNonNegativeLots(s.volume) ||
        !std::isfinite(s.margin) || s.margin < 0.0 ||
        !validHedgeFlag(s.hedgeFlag))
      return false;
  // Preserve pending exposure/reservation identity while replacing held state.
  for (auto &kv : positions_)
    kv.second.longVol = kv.second.longCost = kv.second.shortVol =
        kv.second.shortCost = 0.0;
  for (auto &kv : positions_) {
    kv.second.longCostKnown = kv.second.shortCostKnown = true;
    kv.second.longHeld.clear();
    kv.second.shortHeld.clear();
  }
  for (const auto &s : seeds) {
    Pos &p = positions_[s.instrumentId];
    auto &held = s.isLong ? p.longHeld : p.shortHeld;
    Pos::HeldBucket &bucket = held[s.hedgeFlag];
    bucket.vol += s.volume;
    bucket.cost += s.margin;
    clampFiniteCost(bucket.vol);
    clampFiniteCost(bucket.cost);
    bucket.costKnown = bucket.costKnown &&
                       (s.volume <= 0.0 || s.margin > 0.0);
  }
  for (auto &entry : positions_) {
    Pos &position = entry.second;
    recomputeHeldSideLocked(position, true);
    recomputeHeldSideLocked(position, false);
  }
  // A complete snapshot also proves that instruments absent from it have no
  // held position. Do not retain one empty map entry per instrument ever seen
  // over a long-lived process; working-order entries remain because their
  // pending exposure is still authoritative.
  for (auto it = positions_.begin(); it != positions_.end();) {
    const Pos &position = it->second;
    if (position.longVol <= 0.0 && position.shortVol <= 0.0 &&
        position.longPendVol <= 0.0 && position.shortPendVol <= 0.0)
      it = positions_.erase(it);
    else
      ++it;
  }
  // A same-day exact-dedupe overflow cannot be repaired by a position snapshot:
  // subsequent callbacks from that day are still indistinguishable from old
  // duplicates. Resume only after the stream advances to a later trading day.
  ledgerUncertain_ = !tradeDedupeExhaustedDay_.empty();
  revision_.fetch_add(1, std::memory_order_release);
  return true;
}

void RiskEngine::onTrade(const std::string &tradeKey,
                         const std::string &orderRef,
                         const std::string &orderSysKey,
                         const std::string &instrumentId, bool isBuy,
                         bool isOpen, char hedgeFlag, double price,
                         double volume, const std::string &brokerId,
                         const std::string &investorId) {
  // Real CTP fills are always positive, finite, and far below the sentinel band;
  // ignore malformed data rather than let it corrupt the tracked cost (a NaN
  // would silently void the margin cap, +Inf/DBL_MAX would jam or void it). See
  // sanePositive: the same sentinel rejection as instrument metadata / refPrice.
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  const AccountScope account = accountScopeLocked(brokerId, investorId);
  if (account == AccountScope::Foreign)
    return; // delayed broadcast from another logged-out account
  if (account == AccountScope::Ambiguous) {
    ledgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  if (!sanePositive(price) || !saneLots(volume) ||
      !validHedgeFlag(hedgeFlag)) {
    // A malformed callback may still represent a real fill. Ignoring it would
    // under-count exposure, so require an authoritative position reconciliation.
    ledgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  if (instrumentId.empty()) {
    ledgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  if (tradeKey.empty()) {
    // Without a stable exchange/day identifier we cannot distinguish a replay
    // from a new fill. Applying it could double-count; ignoring it could
    // under-count. Preserve safety by making all position-dependent opening
    // gates fail closed until an authoritative position snapshot arrives.
    ledgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  std::string tradeDay;
  const size_t sep = tradeKey.find('|');
  tradeDay = tradeKey.substr(0, sep);
  if (!validTradingDay(tradeDay)) {
    ledgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  if (!retiredThroughTradeDay_.empty() &&
      tradeDay <= retiredThroughTradeDay_) {
    // Exact ids from this day were intentionally compacted. Silently treating
    // every later callback as a duplicate can miss a real first-seen fill.
    // Preserve bounded memory and safety by requiring a fresh authoritative
    // position sync before any position-dependent opening gate can pass.
    ledgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  const SeenTrade fingerprint{orderRef, orderSysKey, instrumentId, isBuy,
                              isOpen, hedgeFlag, price, volume};
  auto seen = seenTrades_.find(tradeKey);
  if (seen != seenTrades_.end()) {
    const SeenTrade &old = seen->second;
    if (old.orderRef == fingerprint.orderRef &&
        old.orderSysKey == fingerprint.orderSysKey &&
        old.instrumentId == fingerprint.instrumentId &&
        old.isBuy == fingerprint.isBuy && old.isOpen == fingerprint.isOpen &&
        old.hedgeFlag == fingerprint.hedgeFlag &&
        old.price == fingerprint.price && old.volume == fingerprint.volume)
      return; // byte-level semantic duplicate
    // Reusing the same exchange/day trade id for different economics is
    // ambiguous: applying can double-count, ignoring can under-count.
    ledgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }

  if (!tradeDedupeExhaustedDay_.empty()) {
    if (tradeDay <= tradeDedupeExhaustedDay_) {
      ledgerUncertain_ = true;
      revision_.fetch_add(1, std::memory_order_release);
      return;
    }
    // The stream has advanced beyond the exhausted day. Retire every exact key
    // from the old epoch and begin a fresh bounded window, but keep the position
    // ledger uncertain until replacePositionsIfRevision() reconciles fills that
    // were deliberately not guessed while exhausted.
    if (tradeDedupeExhaustedDay_ > retiredThroughTradeDay_)
      retiredThroughTradeDay_ = tradeDedupeExhaustedDay_;
    seenTrades_.clear();
    tradeKeysByDay_.clear();
    tradeDedupeExhaustedDay_.clear();
  }

  // Prefer retiring a complete older day to exhausting the current one. Exact
  // keys from a retired day are intentionally not guessed if replayed later.
  while (seenTrades_.size() >= kMaxSeenTrades &&
         !tradeKeysByDay_.empty() &&
         tradeKeysByDay_.begin()->first < tradeDay) {
    const std::string expired = tradeKeysByDay_.begin()->first;
    if (expired > retiredThroughTradeDay_)
      retiredThroughTradeDay_ = expired;
    for (const auto &key : tradeKeysByDay_.begin()->second)
      seenTrades_.erase(key);
    tradeKeysByDay_.erase(tradeKeysByDay_.begin());
  }
  if (seenTrades_.size() >= kMaxSeenTrades) {
    tradeDedupeExhaustedDay_ = tradeDay;
    if (!tradeKeysByDay_.empty() &&
        tradeKeysByDay_.rbegin()->first > tradeDedupeExhaustedDay_)
      tradeDedupeExhaustedDay_ = tradeKeysByDay_.rbegin()->first;
    ledgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  seenTrades_.emplace(tradeKey, fingerprint);

  // Transfer this fill's exposure from pending to held under the same ledger
  // lock. If OnRtnOrder reported VolumeTraded first, its reservation deliberately
  // included the not-yet-delivered trade; this closes that gap atomically.
  if (isOpen) {
    const std::string reservationKey =
        reservationKeyForTradeLocked(orderSysKey);
    auto rit = reservations_.find(reservationKey);
    if (rit != reservations_.end()) {
      const Reservation &matched = rit->second;
      if (matched.instrumentId != instrumentId || matched.isLong != isBuy ||
          matched.hedgeFlag != hedgeFlag) {
        // The identity index and the economic fields disagree. Apply the fill
        // to held exposure below, but never release the conflicting working
        // order until both ledgers have been authoritatively reconciled.
        orderLedgerUncertain_ = true;
        ledgerUncertain_ = true;
        revision_.fetch_add(1, std::memory_order_release);
        rit = reservations_.end();
      }
    }
    if (rit != reservations_.end()) {
      auto state = orderStates_.find(reservationKey);
      if (state != orderStates_.end()) {
        state->second.appliedTraded += volume;
        clampFiniteCost(state->second.appliedTraded);
      }
      Reservation &reservation = rit->second;
      updateReservationExposureLocked(
          reservation, std::max(0.0, reservation.vol - volume),
          reservation.price);
      if (reservation.vol <= 0.0) {
        Reservation done = reservation;
        unindexReservationLocked(reservationKey, done);
        reservations_.erase(rit);
        pruneEvictedTerminalOrderLocked(reservationKey);
      }
    } else if (!orderSysKey.empty()) {
      // External-terminal ordering can be Trade -> Order. Without this marker,
      // the later terminal Order's VolumeTraded would be counted again as an
      // undelivered fill reservation, leaving a permanent ghost commitment.
      // If an extreme stream never supplies matching Order callbacks, bound the
      // optimization map. Dropping these hints can only over-count a later stale
      // Order as pending; it cannot under-count actual held exposure.
      if (unmatchedFillsBySys_.size() >= kMaxUnmatchedFills &&
          unmatchedFillsBySys_.count(orderSysKey) == 0)
        unmatchedFillsBySys_.clear();
      UnmatchedFill &fill = unmatchedFillsBySys_[orderSysKey];
      if (fill.orderRef.empty() || fill.orderRef == orderRef) {
        fill.orderRef = orderRef;
        fill.volume += volume;
        clampFiniteCost(fill.volume);
      } else {
        // One exchange order id cannot belong to two OrderRefs. The fills are
        // still applied to held exposure, but no reservation may be reconciled
        // through this ambiguous identity until an authoritative order sync.
        orderLedgerUncertain_ = true;
      }
    } else if (!orderRef.empty() && reservationKeysByRef_.count(orderRef) != 0) {
      // We applied the fill to held exposure, but cannot safely choose which
      // front/session reservation to release. Keep it and require order sync.
      orderLedgerUncertain_ = true;
    }
  }

  double cost = 0.0;
  const bool costKnown =
      !isOpen || marginCostLocked(instrumentId, isBuy, hedgeFlag, price,
                                  volume, cost);
  if (isOpen && !costKnown) {
    if (hasCostCapLocked(instrumentId)) {
      // A fill already happened but its metadata is incomplete. Pin the held
      // ledger above every finite cap rather than under-count and fail open.
      cost = std::numeric_limits<double>::max() / 4.0;
    } else {
      // No monetary cap is active: retain a best-effort observable cost while
      // still making future capped opens fail closed at reservation time.
      cost = price * volume * multiplierLocked(instrumentId);
      if (!std::isfinite(cost))
        cost = std::numeric_limits<double>::max() / 4.0;
    }
  }
  Pos &p = positions_[instrumentId];
  if (isOpen) {
    auto &held = isBuy ? p.longHeld : p.shortHeld;
    Pos::HeldBucket &bucket = held[hedgeFlag];
    bucket.costKnown = bucket.vol <= 0.0
                           ? costKnown
                           : bucket.costKnown && costKnown;
    bucket.vol += volume;
    bucket.cost += cost;
    clampFiniteCost(bucket.vol);
    clampFiniteCost(bucket.cost);
    recomputeHeldSideLocked(p, isBuy);
  } else {
    // A close releases only the matching hedge bucket. Averaging speculation
    // and hedge margin together can release too much low-margin exposure and
    // understate the higher-margin remainder.
    const bool closingLong = !isBuy;
    auto &held = closingLong ? p.longHeld : p.shortHeld;
    auto bucketIt = held.find(hedgeFlag);
    if (bucketIt == held.end() || volume > bucketIt->second.vol) {
      ledgerUncertain_ = true;
    }
    if (bucketIt != held.end()) {
      Pos::HeldBucket &bucket = bucketIt->second;
      const double v = std::min(volume, bucket.vol);
      const double rel = bucket.vol > 0.0
                             ? bucket.cost * (v / bucket.vol)
                             : 0.0;
      bucket.cost = std::max(0.0, bucket.cost - rel);
      bucket.vol = std::max(0.0, bucket.vol - v);
      if (bucket.vol <= 0.0)
        held.erase(bucketIt);
    }
    recomputeHeldSideLocked(p, closingLong);
    // A position query and the private realtime stream have no common sequence
    // watermark. This close may already be reflected by the latest snapshot;
    // never let a possible double-release reopen a cap. A fresh authoritative
    // position sync is required before the next opening order.
    ledgerUncertain_ = true;
  }
  auto dit = tradeKeysByDay_.find(tradeDay);
  if (dit == tradeKeysByDay_.end())
    dit = tradeKeysByDay_.emplace(tradeDay, std::vector<std::string>{}).first;
  dit->second.push_back(tradeKey);
  // TradingDay is YYYYMMDD, so lexical order is chronological. A first-seen
  // late old-day fill is applied once and immediately compacted; it cannot
  // rotate a recent exact-key day out of the retention window.
  while (tradeKeysByDay_.size() > kRetainedTradeDays) {
    const std::string expired = tradeKeysByDay_.begin()->first;
    if (expired > retiredThroughTradeDay_)
      retiredThroughTradeDay_ = expired;
    for (const auto &key : tradeKeysByDay_.begin()->second)
      seenTrades_.erase(key);
    tradeKeysByDay_.erase(tradeKeysByDay_.begin());
  }
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::markTradeLedgerUncertain() {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  ledgerUncertain_ = true;
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::markOrderLedgerUncertain() {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  orderLedgerUncertain_ = true;
  revision_.fetch_add(1, std::memory_order_release);
}

double RiskEngine::accountMargin() const {
  std::lock_guard<std::mutex> lk(posMutex_);
  double total = 0.0;
  for (const auto &kv : positions_) {
    total += kv.second.longCost + kv.second.shortCost;
    clampFiniteCost(total);
  }
  return total;
}

bool RiskEngine::setInstrumentMarginLimits(
    const std::vector<InstrumentMarginLimitUpdate> &updates) {
  for (const auto &u : updates)
    if (u.instrumentId.empty() || !std::isfinite(u.limit) || u.limit < 0.0)
      return false;
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  for (const auto &u : updates) {
    const double limit = positiveLimitOrDisabled(u.limit);
    if (limit > 0.0)
      maxInstrumentMargin_[u.instrumentId] = limit;
    else
      maxInstrumentMargin_.erase(u.instrumentId);
  }
  return true;
}

RiskEngine::AccountScope RiskEngine::accountScopeLocked(
    const std::string &brokerId, const std::string &investorId) const {
  if (!sessionIdentityKnown_)
    return AccountScope::Ambiguous;
  // Standalone C++ users may install a numeric session without account text.
  // Real Trader sessions always carry both strings and therefore take the
  // strict branch below.
  if (myBrokerId_.empty() && myInvestorId_.empty())
    return AccountScope::Current;
  if (brokerId.empty() || investorId.empty())
    return AccountScope::Ambiguous;
  return brokerId == myBrokerId_ && investorId == myInvestorId_
             ? AccountScope::Current
             : AccountScope::Foreign;
}

void RiskEngine::clearAccountMarginMetadataLocked() {
  activeMarginQueryMembers_.clear();
  invalidStagedMarginKeys_.clear();
  marginRates_.clear();
  exchangeMarginRates_.clear();
  activeMarginQueryRequestId_ = 0;
  activeMarginQueryKind_ = MarginQueryKind::None;
  activeMarginQueryAuthorized_ = false;
  activeMarginQueryInstrument_.clear();
  activeMarginQueryHedgeFlag_ = '\0';
  stagedMarginRates_.clear();
  invalidateHeldCostsLocked("");
  refreshAllReservationCostsLocked();
}

void RiskEngine::clearAccountLedgerLocked() {
  positions_.clear();
  reservations_.clear();
  insertRequests_.clear();
  insertRequestOrder_.clear();
  orderStates_.clear();
  unsupportedWorkingOrders_.clear();
  terminalOrderKeys_.clear();
  terminalOrderKeySet_.clear();
  reservationKeysByRef_.clear();
  reservationKeyBySys_.clear();
  unmatchedFillsBySys_.clear();
  seenTrades_.clear();
  tradeKeysByDay_.clear();
  retiredThroughTradeDay_.clear();
  tradeDedupeExhaustedDay_.clear();
  unsupportedRiskMutationUsed_ = false;
  clearAccountMarginMetadataLocked();
}

void RiskEngine::invalidateSession() {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  sessionIdentityRequired_ = true;
  sessionIdentityKnown_ = false;
  loginSessionKnown_ = false;
  pendingSessionRequestId_ = 0;
  pendingSessionExpectsLogin_ = false;
  pendingSessionBrokerId_.clear();
  pendingSessionUserId_.clear();
  clearAccountMarginMetadataLocked();
  ledgerUncertain_ = true;
  orderLedgerUncertain_ = true;
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::beginSessionTransition(int requestId, bool expectsLogin,
                                        const std::string &brokerId,
                                        const std::string &userId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  sessionIdentityRequired_ = true;
  sessionIdentityKnown_ = false;
  loginSessionKnown_ = false;
  pendingSessionRequestId_ = requestId > 0 ? requestId : 0;
  pendingSessionExpectsLogin_ = expectsLogin && requestId > 0;
  pendingSessionBrokerId_ = brokerId;
  pendingSessionUserId_ = userId;
  clearAccountMarginMetadataLocked();
  ledgerUncertain_ = true;
  orderLedgerUncertain_ = true;
  revision_.fetch_add(1, std::memory_order_release);
}

bool RiskEngine::acceptLoginSession(int requestId, int frontId, int sessionId,
                                    const std::string &brokerId,
                                    const std::string &userId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (requestId <= 0 || requestId != pendingSessionRequestId_ ||
      !pendingSessionExpectsLogin_ || brokerId.empty() || userId.empty() ||
      frontId <= 0 || sessionId == 0 ||
      (!pendingSessionBrokerId_.empty() &&
       brokerId != pendingSessionBrokerId_) ||
      (!pendingSessionUserId_.empty() && userId != pendingSessionUserId_))
    return false;
  const bool accountChanged =
      !myBrokerId_.empty() && brokerId != myBrokerId_;
  const bool changed = accountChanged || frontId != myFrontId_ ||
                       sessionId != mySessionId_;
  if (accountChanged)
    clearAccountLedgerLocked();
  myFrontId_ = frontId;
  mySessionId_ = sessionId;
  myBrokerId_ = brokerId;
  myLoginUserId_ = userId;
  sessionIdentityRequired_ = true;
  sessionIdentityKnown_ = false;
  loginSessionKnown_ = true;
  pendingSessionRequestId_ = 0;
  pendingSessionExpectsLogin_ = false;
  pendingSessionBrokerId_.clear();
  pendingSessionUserId_.clear();
  if (changed) {
    ledgerUncertain_ = true;
    orderLedgerUncertain_ = true;
  }
  revision_.fetch_add(1, std::memory_order_release);
  return true;
}

bool RiskEngine::confirmSessionAccount(
    int frontId, int sessionId, const std::string &brokerId,
    const std::string &investorId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (!loginSessionKnown_ || frontId <= 0 || sessionId == 0 ||
      frontId != myFrontId_ || sessionId != mySessionId_ || brokerId.empty() ||
      investorId.empty() || brokerId != myBrokerId_)
    return false;
  const bool accountChanged =
      !myInvestorId_.empty() && investorId != myInvestorId_;
  if (accountChanged)
    clearAccountLedgerLocked();
  myInvestorId_ = investorId;
  sessionIdentityRequired_ = true;
  sessionIdentityKnown_ = true;
  ledgerUncertain_ = true;
  orderLedgerUncertain_ = true;
  revision_.fetch_add(1, std::memory_order_release);
  return true;
}

bool RiskEngine::acceptsOrderAccount(const std::string &brokerId,
                                     const std::string &investorId) const {
  std::lock_guard<std::mutex> lk(posMutex_);
  return accountScopeLocked(brokerId, investorId) == AccountScope::Current;
}

void RiskEngine::finishSessionTransition(int requestId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (requestId <= 0 || requestId != pendingSessionRequestId_)
    return;
  pendingSessionRequestId_ = 0;
  pendingSessionExpectsLogin_ = false;
  pendingSessionBrokerId_.clear();
  pendingSessionUserId_.clear();
  revision_.fetch_add(1, std::memory_order_release);
}

bool RiskEngine::timeoutSessionTransition(int requestId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (requestId <= 0 || requestId != pendingSessionRequestId_)
    return false;
  pendingSessionRequestId_ = 0;
  pendingSessionExpectsLogin_ = false;
  pendingSessionBrokerId_.clear();
  pendingSessionUserId_.clear();
  sessionIdentityKnown_ = false;
  loginSessionKnown_ = false;
  ledgerUncertain_ = true;
  orderLedgerUncertain_ = true;
  revision_.fetch_add(1, std::memory_order_release);
  return true;
}

void RiskEngine::setSession(int frontId, int sessionId,
                            const std::string &brokerId,
                            const std::string &investorId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  const bool concreteIdentity = !sessionIdentityRequired_ ||
                                (!brokerId.empty() && !investorId.empty());
  const bool changed = sessionIdentityKnown_ &&
      (frontId != myFrontId_ || sessionId != mySessionId_ ||
       (!brokerId.empty() && !myBrokerId_.empty() && brokerId != myBrokerId_) ||
       (!investorId.empty() && !myInvestorId_.empty() &&
        investorId != myInvestorId_));
  const bool accountChanged = sessionIdentityKnown_ &&
      ((!brokerId.empty() && !myBrokerId_.empty() &&
        brokerId != myBrokerId_) ||
       (!investorId.empty() && !myInvestorId_.empty() &&
        investorId != myInvestorId_));
  if (accountChanged)
    clearAccountLedgerLocked();
  myFrontId_ = frontId;
  mySessionId_ = sessionId;
  myBrokerId_ = brokerId;
  myInvestorId_ = investorId;
  myLoginUserId_ = investorId;
  sessionIdentityRequired_ = true;
  sessionIdentityKnown_ = concreteIdentity;
  loginSessionKnown_ = concreteIdentity;
  pendingSessionRequestId_ = 0;
  pendingSessionExpectsLogin_ = false;
  pendingSessionBrokerId_.clear();
  pendingSessionUserId_.clear();
  if (changed || !concreteIdentity) {
    // A relogin can switch sessions/accounts without a disconnect callback.
    // Never carry the old account's held/working certainty into the new one.
    ledgerUncertain_ = true;
    orderLedgerUncertain_ = true;
  }
  if (changed || !concreteIdentity)
    clearAccountMarginMetadataLocked();
  revision_.fetch_add(1, std::memory_order_release);
}

std::string RiskEngine::resvKey(int frontId, int sessionId,
                                const std::string &orderRef) {
  return std::to_string(frontId) + ":" + std::to_string(sessionId) + ":" + orderRef;
}

void RiskEngine::rememberTerminalOrderLocked(const std::string &key) {
  if (!terminalOrderKeySet_.insert(key).second)
    return;
  terminalOrderKeys_.push_back(key);
  while (terminalOrderKeys_.size() > kMaxTerminalOrderTombstones) {
    std::string expired = std::move(terminalOrderKeys_.front());
    terminalOrderKeys_.pop_front();
    terminalOrderKeySet_.erase(expired);
    auto state = orderStates_.find(expired);
    if (state != orderStates_.end() && state->second.terminal &&
        reservations_.count(expired) == 0)
      orderStates_.erase(state);
  }
}

void RiskEngine::forgetTerminalOrderLocked(const std::string &key) {
  if (terminalOrderKeySet_.erase(key) == 0)
    return;
  terminalOrderKeys_.erase(
      std::remove(terminalOrderKeys_.begin(), terminalOrderKeys_.end(), key),
      terminalOrderKeys_.end());
}

void RiskEngine::pruneEvictedTerminalOrderLocked(const std::string &key) {
  if (terminalOrderKeySet_.count(key) != 0 || reservations_.count(key) != 0)
    return;
  auto state = orderStates_.find(key);
  if (state != orderStates_.end() && state->second.terminal)
    orderStates_.erase(state);
}

OpenGate RiskEngine::tryReserveOpen(const std::string &orderRef,
                                    const std::string &instrumentId, bool isLong,
                                    double price, double volume,
                                    char hedgeFlag) {
  const double globalCap = maxAccountMargin_.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(posMutex_);
  const std::string key = resvKey(myFrontId_, mySessionId_, orderRef);
  // OrderRef is an idempotency key within a front/session. Never overwrite an
  // existing reservation/state: doing so loses the original ledger entry and
  // leaves a phantom pending amount after the duplicate is rejected.
  if (orderRef.empty() || reservations_.find(key) != reservations_.end() ||
      orderStates_.find(key) != orderStates_.end())
    return OpenGate::DuplicateOrderRef;
  if (sessionIdentityRequired_ && !sessionIdentityKnown_)
    return OpenGate::MetadataUnavailable;
  if (instrumentId.empty() || !saneLots(volume) || !validHedgeFlag(hedgeFlag) ||
      !std::isfinite(price) || price < 0.0)
    return OpenGate::MetadataUnavailable;

  auto vit = maxPositionVol_.find(instrumentId);
  auto cit = maxInstrumentMargin_.find(instrumentId);
  if (ledgerUncertain_ || orderLedgerUncertain_ ||
      !unsupportedWorkingOrders_.empty())
    return OpenGate::MetadataUnavailable;
  if (invalidConfig_.load(std::memory_order_acquire))
    return OpenGate::MetadataUnavailable;
  if (unsupportedRiskMutationUsed_ && localControlsEnabledLocked())
    return OpenGate::MetadataUnavailable;

  // Revalue every existing working order before comparing a monetary cap. A
  // once-fresh reference cannot remain authoritative for a long-lived order as
  // the market moves; a stale/missing current quote turns it unknown.
  if (globalCap > 0.0)
    refreshAllReservationCostsLocked();
  else if (cit != maxInstrumentMargin_.end() && cit->second > 0.0)
    refreshReservationCostsLocked(instrumentId);

  const double reference = refPrice(instrumentId);
  const bool priceKnown = sanePositive(price) && sanePositive(reference);
  const double valuationPrice = priceKnown ? std::max(price, reference) : 0.0;
  double addCost = 0.0;
  const bool costKnown = priceKnown &&
      marginCostLocked(instrumentId, isLong, hedgeFlag, valuationPrice, volume,
                       addCost);
  // Revaluation may insert a missing positions_ entry and rehash the map, so
  // take this pointer only after all refreshes are complete.
  const Pos *p = nullptr;
  auto pit = positions_.find(instrumentId);
  if (pit != positions_.end())
    p = &pit->second;

  // 1) per-side volume cap (committed = held + reserved on this side)
  if (vit != maxPositionVol_.end()) {
    const double cap = isLong ? vit->second.longMax : vit->second.shortMax;
    if (cap > 0.0) {
      double committed = 0.0;
      if (p)
        committed = isLong ? p->longVol + p->longPendVol
                           : p->shortVol + p->shortPendVol;
      if (committed + volume > cap)
        return OpenGate::VolumeLimit;
    }
  }

  // 2) per-instrument margin cap (both sides, held + reserved)
  if (cit != maxInstrumentMargin_.end() && cit->second > 0.0) {
    double instrCost = 0.0;
    if (p)
      instrCost = p->longCost + p->longPendCost + p->shortCost + p->shortPendCost;
    if (instrCost >= cit->second)
      return OpenGate::CostLimit;
    if ((p && ((p->longVol > 0.0 && !p->longCostKnown) ||
               (p->shortVol > 0.0 && !p->shortCostKnown) ||
               p->longPendUnknown > 0 || p->shortPendUnknown > 0)) ||
        !costKnown)
      return OpenGate::MetadataUnavailable;
    if (addCost > cit->second - instrCost)
      return OpenGate::CostLimit;
  }

  // 3) account-wide margin cap (whole book, held + reserved)
  if (globalCap > 0.0) {
    double total = 0.0;
    for (const auto &kv : positions_)
      total += kv.second.longCost + kv.second.longPendCost +
               kv.second.shortCost + kv.second.shortPendCost;
    if (total >= globalCap)
      return OpenGate::CostLimit;
    for (const auto &kv : positions_)
      if ((kv.second.longVol > 0.0 && !kv.second.longCostKnown) ||
          (kv.second.shortVol > 0.0 && !kv.second.shortCostKnown) ||
          kv.second.longPendUnknown > 0 || kv.second.shortPendUnknown > 0)
        return OpenGate::MetadataUnavailable;
    if (!costKnown)
      return OpenGate::MetadataUnavailable;
    if (addCost > globalCap - total)
      return OpenGate::CostLimit;
  }

  // Track every accepted open, even when no cap is currently active. A cap
  // enabled later must still see already-working orders.
  Pos &pp = positions_[instrumentId];
  if (isLong) {
    pp.longPendVol += volume;
    pp.longPendCost += costKnown ? addCost : 0.0;
    clampFiniteCost(pp.longPendVol);
    clampFiniteCost(pp.longPendCost);
    if (!costKnown)
      ++pp.longPendUnknown;
  } else {
    pp.shortPendVol += volume;
    pp.shortPendCost += costKnown ? addCost : 0.0;
    clampFiniteCost(pp.shortPendVol);
    clampFiniteCost(pp.shortPendCost);
    if (!costKnown)
      ++pp.shortPendUnknown;
  }
  reservations_[key] = Reservation{orderRef, "", instrumentId, isLong,
                                   hedgeFlag, volume,
                                   costKnown ? addCost : 0.0, price, costKnown};
  indexReservationLocked(key);
  orderStates_[key] = OrderState{false, volume, 0.0, 0.0, ""};
  revision_.fetch_add(1, std::memory_order_release);
  return OpenGate::Ok;
}

void RiskEngine::onOrderUpdate(int frontId, int sessionId,
                               const std::string &orderRef,
                               const std::string &instrumentId, bool isOpen,
                               bool isLong, char hedgeFlag, char status,
                               double limitPrice, double volTotal,
                               double volTraded,
                               const std::string &orderSysKey,
                               const std::string &brokerId,
                               const std::string &investorId) {
  if (!isOpen)
    return; // only opening orders reserve
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  const AccountScope account = accountScopeLocked(brokerId, investorId);
  if (account == AccountScope::Foreign)
    return;
  if (account == AccountScope::Ambiguous) {
    orderLedgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  if (orderRef.empty() || instrumentId.empty() || !validHedgeFlag(hedgeFlag) ||
      (limitPrice != 0.0 && !sanePositive(limitPrice)) ||
      !saneNonNegativeLots(volTotal) ||
      !saneNonNegativeLots(volTraded)) {
    // For an opening order from this or another terminal, silently dropping a
    // malformed update can omit working exposure. Preserve safety until the
    // authoritative working-order snapshot is rebuilt.
    orderLedgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  const std::string key = resvKey(frontId, sessionId, orderRef);
  if (!orderSysKey.empty()) {
    auto indexed = reservationKeyBySys_.find(orderSysKey);
    if (indexed != reservationKeyBySys_.end() && indexed->second != key) {
      // Exchange order identity must be one-to-one. Overwriting this index
      // could make a later Trade release the wrong terminal/session order.
      orderLedgerUncertain_ = true;
      revision_.fetch_add(1, std::memory_order_release);
      return;
    }
  }
  // The order still holds live working volume UNLESS it has reached a terminal
  // state. Release only on a definitively-done status: AllTraded '0',
  // PartTradedNotQueueing '2', NoTradeNotQueueing '4', Canceled '5'. Everything
  // else: queueing ('1'/'3') and the transient pre-trade states a conditional
  // order can report before '3' (Unknown 'a', NotTouched 'b', Touched 'c'), plus
  // any status CTP might add later keeps the reservation. Releasing on a
  // transient status that merely precedes '3' would briefly drop the cap for
  // this order's volume (a burst could then slip past). Conservative by design:
  // hold the reservation until the order is provably finished.
  const bool terminal = terminalOrderStatus(status);
  if (volTraded > volTotal) {
    // The malformed row may describe fills and/or working exposure larger than
    // the original order. Retaining one old reservation is not a complete bound.
    orderLedgerUncertain_ = true;
    ledgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  const double working = terminal ? 0.0 : volTotal - volTraded;

  auto rit = reservations_.find(key);
  if (rit != reservations_.end()) {
    const Reservation &r = rit->second;
    if (r.instrumentId != instrumentId || r.isLong != isLong ||
        r.hedgeFlag != hedgeFlag ||
        (!r.orderSysKey.empty() && !orderSysKey.empty() &&
         r.orderSysKey != orderSysKey)) {
      // Retaining the old reservation alone is not enough: the conflicting
      // callback may describe a larger or different live order. Require a full
      // order reconciliation before any further opening insert.
      orderLedgerUncertain_ = true;
      revision_.fetch_add(1, std::memory_order_release);
      return;
    }
  }

  auto sit = orderStates_.find(key);
  if (sit != orderStates_.end()) {
    OrderState &s = sit->second;
    if (s.terminal) {
      if (terminal) {
        // Only a matching concrete exchange identity proves this is a replay.
        // A terminal callback with a different/missing fingerprint may be a
        // newly completed order after front/session/ref reuse; ignoring it can
        // omit its not-yet-delivered fill reservation.
        if (!s.orderSysKey.empty() && !orderSysKey.empty() &&
            s.orderSysKey == orderSysKey)
          return;
        orderLedgerUncertain_ = true;
        revision_.fetch_add(1, std::memory_order_release);
        return;
      }
      // A concrete matching exchange id proves this is a stale replay of the
      // terminal order. A different/missing fingerprint is ambiguous: front,
      // session and OrderRef can be reused after a long-lived client's front
      // reconnect, so silently ignoring it could omit a genuinely new order.
      if (!s.orderSysKey.empty() && !orderSysKey.empty() &&
          s.orderSysKey == orderSysKey)
        return;
      orderLedgerUncertain_ = true;
      revision_.fetch_add(1, std::memory_order_release);
      return;
    }
    if (!s.orderSysKey.empty() && !orderSysKey.empty() &&
        s.orderSysKey != orderSysKey) {
      orderLedgerUncertain_ = true;
      revision_.fetch_add(1, std::memory_order_release);
      return;
    }
    if (!terminal) {
      // Working remainder and traded volume should be monotonic for an ordinary
      // order. A Modify action can legitimately increase the remainder, while
      // an out-of-order replay can appear identical. Do not guess: retain the
      // conservative prior exposure and block opens until syncOrders().
      if (volTraded < s.reportedTraded || working > s.remaining) {
        orderLedgerUncertain_ = true;
        revision_.fetch_add(1, std::memory_order_release);
        return;
      }
    }
    s.terminal = terminal;
    s.remaining = working;
    s.reportedTraded = std::max(s.reportedTraded, volTraded);
    if (s.orderSysKey.empty() && !orderSysKey.empty())
      s.orderSysKey = orderSysKey;
  } else {
    sit = orderStates_
              .emplace(key, OrderState{terminal, working, volTraded, 0.0,
                                       orderSysKey})
              .first;
  }
  OrderState &state = sit->second;
  if (terminal)
    rememberTerminalOrderLocked(key);
  if (!orderSysKey.empty()) {
    auto unmatched = unmatchedFillsBySys_.find(orderSysKey);
    if (unmatched != unmatchedFillsBySys_.end()) {
      if (!unmatched->second.orderRef.empty() &&
          unmatched->second.orderRef != orderRef) {
        orderLedgerUncertain_ = true;
        revision_.fetch_add(1, std::memory_order_release);
        return;
      }
      state.appliedTraded += unmatched->second.volume;
      clampFiniteCost(state.appliedTraded);
      unmatchedFillsBySys_.erase(unmatched);
    }
  }
  if (rit != reservations_.end() &&
      observeInsertRequestLocked(key, rit->second)) {
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  double desiredVol =
      working + std::max(0.0, state.reportedTraded - state.appliedTraded);
  clampFiniteCost(desiredVol);

  if (rit == reservations_.end()) {
    // Track every working or not-yet-delivered fill, including orders sent
    // before a cap existed and orders placed by another terminal.
    if (desiredVol <= 0.0) {
      revision_.fetch_add(1, std::memory_order_release);
      return;
    }
    reservations_[key] = Reservation{orderRef, orderSysKey, instrumentId,
                                     isLong, hedgeFlag, 0.0, 0.0, limitPrice,
                                     true};
    indexReservationLocked(key);
    updateReservationExposureLocked(reservations_[key], desiredVol,
                                    limitPrice);
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }

  // Reconcile to working remainder + fills reported by Order but not yet applied
  // by Trade. This makes either callback ordering conservative and gap-free.
  Reservation &r = rit->second;
  if (r.orderSysKey.empty() && !orderSysKey.empty()) {
    r.orderSysKey = orderSysKey;
    reservationKeyBySys_[orderSysKey] = key;
  }
  updateReservationExposureLocked(r, desiredVol, limitPrice);
  if (desiredVol <= 0.0) {
    Reservation done = r;
    unindexReservationLocked(key, done);
    reservations_.erase(rit);
  }
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::onUnsupportedOrderUpdate(int frontId, int sessionId,
                                           const std::string &orderRef,
                                           char status,
                                           const std::string &orderSysKey,
                                           const std::string &brokerId,
                                           const std::string &investorId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  const AccountScope account = accountScopeLocked(brokerId, investorId);
  if (account == AccountScope::Foreign)
    return;
  if (account == AccountScope::Ambiguous) {
    orderLedgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  if (orderRef.empty()) {
    orderLedgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  const std::string key = resvKey(frontId, sessionId, orderRef);
  auto boundReservation = reservations_.find(key);
  if (boundReservation != reservations_.end() &&
      observeInsertRequestLocked(key, boundReservation->second)) {
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  bool changed = false;
  auto state = orderStates_.find(key);
  if (state != orderStates_.end()) {
    if (state->second.terminal && terminalOrderStatus(status)) {
      if (!state->second.orderSysKey.empty() && !orderSysKey.empty() &&
          state->second.orderSysKey == orderSysKey)
        return; // proven replay of the same terminal order
      orderLedgerUncertain_ = true;
      revision_.fetch_add(1, std::memory_order_release);
      return;
    }
    const std::string &knownSys = state->second.orderSysKey;
    if (!knownSys.empty() && !orderSysKey.empty() && knownSys != orderSysKey) {
      orderLedgerUncertain_ = true;
      revision_.fetch_add(1, std::memory_order_release);
      return;
    }
    if (state->second.terminal && !terminalOrderStatus(status)) {
      if (!knownSys.empty() && !orderSysKey.empty() && knownSys == orderSysKey)
        return; // known stale replay
      orderLedgerUncertain_ = true;
      revision_.fetch_add(1, std::memory_order_release);
      return;
    }
  }
  if (terminalOrderStatus(status)) {
    auto reservation = reservations_.find(key);
    if (reservation != reservations_.end()) {
      updateReservationExposureLocked(reservation->second, 0.0,
                                      reservation->second.price);
      Reservation done = reservation->second;
      unindexReservationLocked(key, done);
      reservations_.erase(reservation);
      changed = true;
    }
    changed = unsupportedWorkingOrders_.erase(key) != 0 || changed;
    if (state == orderStates_.end()) {
      orderStates_[key] = OrderState{true, 0.0, 0.0, 0.0, orderSysKey};
      changed = true;
    } else if (!state->second.terminal) {
      state->second.terminal = true;
      state->second.remaining = 0.0;
      if (state->second.orderSysKey.empty() && !orderSysKey.empty())
        state->second.orderSysKey = orderSysKey;
      changed = true;
    }
    rememberTerminalOrderLocked(key);
  } else {
    if (state == orderStates_.end())
      orderStates_[key] = OrderState{false, 0.0, 0.0, 0.0, orderSysKey};
    else if (state->second.orderSysKey.empty() && !orderSysKey.empty())
      state->second.orderSysKey = orderSysKey;
    changed = unsupportedWorkingOrders_.insert(key).second || changed;
  }
  if (changed)
    revision_.fetch_add(1, std::memory_order_release);
}

bool RiskEngine::bindInsertRequest(int requestId,
                                   const std::string &orderRef) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  const std::string key = resvKey(myFrontId_, mySessionId_, orderRef);
  auto rit = reservations_.find(key);
  const bool bindable = requestId > 0 && !orderRef.empty() &&
      rit != reservations_.end() && rit->second.orderRef == orderRef &&
      rit->second.insertBindingToken == 0 &&
      insertRequests_.find(requestId) == insertRequests_.end();
  if (!bindable) {
    // The request has not reached CTP yet. Roll back only the exact unbound
    // reservation just created by this admission attempt; never disturb a
    // reservation already owned by another request.
    if (rit != reservations_.end() && rit->second.orderRef == orderRef &&
        rit->second.insertBindingToken == 0) {
      updateReservationExposureLocked(rit->second, 0.0, rit->second.price);
      Reservation undone = rit->second;
      unindexReservationLocked(key, undone);
      reservations_.erase(rit);
      auto state = orderStates_.find(key);
      if (state != orderStates_.end() && !state->second.terminal)
        orderStates_.erase(state);
      revision_.fetch_add(1, std::memory_order_release);
    }
    return false;
  }

  uint64_t token = nextInsertBindingToken_++;
  if (token == 0) { // practically unreachable wrap; zero is the unbound marker
    token = nextInsertBindingToken_++;
    if (token == 0)
      token = 1;
  }
  rit->second.insertRequestId = requestId;
  rit->second.insertBindingToken = token;
  insertRequests_.emplace(
      requestId, InsertRequestBinding{key, orderRef, token,
                                      InsertRequestState::Pending});
  insertRequestOrder_.emplace_back(requestId, token);
  while (insertRequestOrder_.size() > kMaxInsertRequestBindings) {
    const auto expired = insertRequestOrder_.front();
    insertRequestOrder_.pop_front();
    auto old = insertRequests_.find(expired.first);
    if (old != insertRequests_.end() && old->second.token == expired.second)
      insertRequests_.erase(old);
  }
  return true;
}

void RiskEngine::rejectInsertByRequest(int requestId,
                                       const std::string &orderRef,
                                       const std::string &brokerId,
                                       const std::string &investorId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  const AccountScope account = accountScopeLocked(brokerId, investorId);
  if (account == AccountScope::Foreign)
    return;
  if (account == AccountScope::Ambiguous) {
    orderLedgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  auto binding = insertRequests_.find(requestId);
  if (binding == insertRequests_.end() ||
      (!orderRef.empty() && binding->second.orderRef != orderRef)) {
    // The response cannot be attributed to one exact generation. It may be an
    // evicted/very-late response; guessing the current session would fail open.
    orderLedgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  InsertRequestBinding &bound = binding->second;
  const std::string &boundOrderRef = bound.orderRef;
  if (bound.state == InsertRequestState::Rejected)
    return; // idempotent duplicate rejection

  auto reservation = reservations_.find(bound.reservationKey);
  const bool ownsReservation = reservation != reservations_.end() &&
      reservation->second.orderRef == boundOrderRef &&
      reservation->second.insertRequestId == requestId &&
      reservation->second.insertBindingToken == bound.token;
  auto state = orderStates_.find(bound.reservationKey);
  const bool hasLiveState = state != orderStates_.end() &&
                            !state->second.terminal;

  if (bound.state == InsertRequestState::Pending && ownsReservation) {
    updateReservationExposureLocked(reservation->second, 0.0,
                                    reservation->second.price);
    Reservation rejected = reservation->second;
    unindexReservationLocked(bound.reservationKey, rejected);
    reservations_.erase(reservation);
    OrderState &terminal = orderStates_[bound.reservationKey];
    terminal.terminal = true;
    terminal.remaining = 0.0;
    if (terminal.orderSysKey.empty() && !rejected.orderSysKey.empty())
      terminal.orderSysKey = rejected.orderSysKey;
    rememberTerminalOrderLocked(bound.reservationKey);
    bound.state = InsertRequestState::Rejected;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }

  if (ownsReservation || hasLiveState ||
      (reservation != reservations_.end() &&
       reservation->second.orderRef == boundOrderRef)) {
    // An OnRtnOrder already proved this generation live, or the same key has
    // since been reused/rebuilt with different ownership. Never delete it.
    orderLedgerUncertain_ = true;
    bound.state = InsertRequestState::Rejected;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }

  // An authoritative snapshot or terminal OnRtnOrder has already removed this
  // generation. Remember the rejection so a replay stays idempotent.
  bound.state = InsertRequestState::Rejected;
}

void RiskEngine::rejectInsertUnscoped(const std::string &orderRef,
                                      const std::string &brokerId,
                                      const std::string &investorId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  const AccountScope account = accountScopeLocked(brokerId, investorId);
  if (account == AccountScope::Foreign)
    return;
  if (account == AccountScope::Ambiguous) {
    orderLedgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  bool live = false;
  if (!orderRef.empty()) {
    auto refs = reservationKeysByRef_.find(orderRef);
    live = refs != reservationKeysByRef_.end() && !refs->second.empty();
  } else {
    live = !reservations_.empty();
  }
  if (!live) {
    for (const auto &entry : insertRequests_) {
      const InsertRequestBinding &binding = entry.second;
      if (binding.state == InsertRequestState::Rejected ||
          (!orderRef.empty() && binding.orderRef != orderRef))
        continue;
      auto reservation = reservations_.find(binding.reservationKey);
      auto state = orderStates_.find(binding.reservationKey);
      if ((reservation != reservations_.end() &&
           reservation->second.insertBindingToken == binding.token) ||
          (state != orderStates_.end() && !state->second.terminal)) {
        live = true;
        break;
      }
    }
  }
  if (!live)
    return;
  // OnErrRtnOrderInsert supplies neither request id nor session. Retaining the
  // reservation can over-block, but releasing by a reused ref can under-count.
  orderLedgerUncertain_ = true;
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::rollbackInsertRequest(int requestId,
                                       const std::string &orderRef) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  auto binding = insertRequests_.find(requestId);
  if (binding == insertRequests_.end() || orderRef.empty() ||
      binding->second.orderRef != orderRef) {
    orderLedgerUncertain_ = true;
    revision_.fetch_add(1, std::memory_order_release);
    return;
  }
  const InsertRequestBinding bound = binding->second;
  auto reservation = reservations_.find(bound.reservationKey);
  const bool ownsReservation = reservation != reservations_.end() &&
      reservation->second.insertRequestId == requestId &&
      reservation->second.insertBindingToken == bound.token;
  if (bound.state != InsertRequestState::Pending || !ownsReservation) {
    // A callback occurred inline despite a local/nonzero return, or ownership
    // changed unexpectedly. Preserve exposure and reconcile instead of erasing.
    if (bound.state != InsertRequestState::Rejected) {
      orderLedgerUncertain_ = true;
      revision_.fetch_add(1, std::memory_order_release);
    }
    return;
  }
  updateReservationExposureLocked(reservation->second, 0.0,
                                  reservation->second.price);
  Reservation undone = reservation->second;
  unindexReservationLocked(bound.reservationKey, undone);
  reservations_.erase(reservation);
  auto state = orderStates_.find(bound.reservationKey);
  if (state != orderStates_.end() && !state->second.terminal)
    orderStates_.erase(state);
  insertRequests_.erase(binding);
  revision_.fetch_add(1, std::memory_order_release);
}

void RiskEngine::clearPendingLocked() {
  // Authoritative resync: drop all reservations and the in-flight portion of
  // every position, then re-reserve from the live working orders. Held (filled)
  // position is untouched - that's reconciled by syncPositions.
  for (auto &kv : positions_) {
    kv.second.longPendVol = kv.second.longPendCost = 0.0;
    kv.second.shortPendVol = kv.second.shortPendCost = 0.0;
    kv.second.longPendUnknown = kv.second.shortPendUnknown = 0;
  }
  reservations_.clear();
  reservationKeysByRef_.clear();
  reservationKeyBySys_.clear();
  unsupportedWorkingOrders_.clear();
  orderLedgerUncertain_ = false;
  // The authoritative order snapshot carries VolumeTraded for every working
  // row and rebuilds appliedTraded from it, so pre-snapshot Trade->Order hints
  // are no longer needed.
  unmatchedFillsBySys_.clear();
  for (auto it = orderStates_.begin(); it != orderStates_.end();) {
    if (!it->second.terminal || terminalOrderKeySet_.count(it->first) == 0)
      it = orderStates_.erase(it);
    else
      ++it;
  }
}

bool RiskEngine::validOpenOrderSnapshot(
    const std::vector<OpenOrderInfo> &orders) {
  std::unordered_map<std::string, const OpenOrderInfo *> byKey;
  std::unordered_map<std::string, std::string> bySys;
  for (const auto &o : orders) {
    if (o.orderRef.empty() || o.instrumentId.empty() ||
        (o.supportedSingleLeg && !validHedgeFlag(o.hedgeFlag)) ||
        !saneNonNegativeLots(o.vol) || !saneNonNegativeLots(o.traded) ||
        (o.price != 0.0 && !sanePositive(o.price)))
      return false;
    const std::string key = resvKey(o.frontId, o.sessionId, o.orderRef);
    auto [duplicate, inserted] = byKey.emplace(key, &o);
    if (!inserted) {
      const OpenOrderInfo &old = *duplicate->second;
      if (old.instrumentId != o.instrumentId || old.isLong != o.isLong ||
          old.hedgeFlag != o.hedgeFlag || old.vol != o.vol ||
          old.price != o.price || old.traded != o.traded ||
          old.orderSysKey != o.orderSysKey ||
          old.supportedSingleLeg != o.supportedSingleLeg)
        return false;
    }
    if (!o.orderSysKey.empty()) {
      auto [sys, sysInserted] = bySys.emplace(o.orderSysKey, key);
      if (!sysInserted && sys->second != key)
        return false;
    }
  }
  return true;
}

bool RiskEngine::rebuildOpenReservationsLocked(
    const std::vector<OpenOrderInfo> &orders) {
  // Decide every terminal-key collision before clearing live reservations. A
  // matching concrete sys id proves the query row is a stale replay and can be
  // skipped. A different concrete id proves key reuse and may supersede the
  // tombstone. Without both fingerprints the snapshot is ambiguous and must
  // leave the current ledger intact and fail closed.
  for (const auto &o : orders) {
    const std::string key = resvKey(o.frontId, o.sessionId, o.orderRef);
    auto tomb = orderStates_.find(key);
    if (tomb != orderStates_.end() && tomb->second.terminal &&
        (tomb->second.orderSysKey.empty() || o.orderSysKey.empty())) {
      orderLedgerUncertain_ = true;
      return false;
    }
  }
  clearPendingLocked();
  for (const auto &o : orders) {
    const std::string key = resvKey(o.frontId, o.sessionId, o.orderRef);
    auto tomb = orderStates_.find(key);
    if (tomb != orderStates_.end() && tomb->second.terminal) {
      if (tomb->second.orderSysKey == o.orderSysKey)
        continue; // stale working replay of this exact terminal order
      // This snapshot was revision-checked against every realtime callback and
      // is authoritative for currently working orders. A live row with a reused
      // front/session/ref and a different concrete exchange id therefore
      // supersedes an older terminal tombstone.
      forgetTerminalOrderLocked(key);
      orderStates_.erase(tomb);
    }
    if (!o.supportedSingleLeg) {
      unsupportedWorkingOrders_.insert(key);
      orderStates_[key] = OrderState{false, 0.0, 0.0, 0.0,
                                     o.orderSysKey};
      continue;
    }
    if (o.vol <= 0.0)
      continue; // a zero working remainder contributes no exposure
    // Dedupe by key: reqQryOrder can return the same working order more than once
    // (paged / duplicate rows). Each (front:session:ref) must reserve exactly
    // once; a duplicate would inflate the pending counters while reservations_
    // collapses to a single entry, leaving a phantom reservation with no map
    // entry to release it - it would over-block opens until the next full resync.
    if (reservations_.count(key))
      continue;
    reservations_[key] = Reservation{o.orderRef, o.orderSysKey, o.instrumentId,
                                     o.isLong, o.hedgeFlag, 0.0, 0.0, o.price,
                                     true};
    indexReservationLocked(key);
    // Use the same conservative valuation as realtime order updates: max of a
    // positive limit and a fresh reference, with missing/stale price or margin
    // metadata represented as unknown so monetary gates fail closed.
    updateReservationExposureLocked(reservations_[key], o.vol, o.price);
    orderStates_[key] =
        OrderState{false, o.vol, o.traded, o.traded, o.orderSysKey};
  }
  return true;
}

void RiskEngine::rebuildOpenReservations(
    const std::vector<OpenOrderInfo> &orders) {
  if (!validOpenOrderSnapshot(orders)) {
    markOrderLedgerUncertain();
    return;
  }
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  (void)rebuildOpenReservationsLocked(orders);
  revision_.fetch_add(1, std::memory_order_release);
}

bool RiskEngine::rebuildOpenReservationsIfRevision(
    uint64_t expected, const std::vector<OpenOrderInfo> &orders) {
  if (!validOpenOrderSnapshot(orders)) {
    markOrderLedgerUncertain();
    return false;
  }
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (revision_.load(std::memory_order_relaxed) != expected)
    return false;
  if (!rebuildOpenReservationsLocked(orders)) {
    revision_.fetch_add(1, std::memory_order_release);
    return false;
  }
  revision_.fetch_add(1, std::memory_order_release);
  return true;
}

RiskVerdict RiskEngine::block(const char *reason) const {
  lastReason_ = reason;
  return {false, reason};
}

RiskVerdict RiskEngine::check(const std::string &instrumentId, double price,
                              double refPrice, int volume, bool isOpen) const {
  if (volume <= 0)
    return block("order volume must be positive");

  // Reject a non-finite limit price (NaN/Inf) here, at the validation point, so
  // it can't reach tryReserveOpen and poison the reserved cost. A market/any-
  // price order legitimately has price 0 (finite), so isfinite allows it.
  if (!std::isfinite(price) || price < 0.0)
    return block("order price must be finite and non-negative");

  const RiskMode currentMode = mode_.load(std::memory_order_acquire);
  if (currentMode == RiskMode::HaltAll)
    return block("all order inserts halted");

  // Closing orders reduce risk and remain available in ReduceOnly mode and
  // during metadata outages. Only basic structural validation applies to them.
  if (!isOpen) {
    lastReason_ = nullptr;
    return {true, nullptr};
  }

  if (invalidConfig_.load(std::memory_order_acquire))
    return block("risk configuration is invalid");

  if (currentMode == RiskMode::ReduceOnly)
    return block("opening orders blocked in reduce-only mode");

  const int maxVol = maxOrderVolume_.load(std::memory_order_relaxed);
  if (maxVol > 0 && volume > maxVol)
    return block("order volume exceeds maxOrderVolume");

  const double dev = maxPriceDeviation_.load(std::memory_order_relaxed);
  if (dev > 0.0) {
    if (!sanePositive(refPrice))
      return block("reference price unavailable or stale");
    if (!sanePositive(price))
      return block("order price unavailable for deviation check");
    const double diff = std::fabs(price - refPrice) / refPrice;
    if (diff > dev)
      return block("order price deviates too far from reference");
  }

  const double maxNotional = maxNotional_.load(std::memory_order_relaxed);
  if (maxNotional > 0.0) {
    if (!sanePositive(refPrice))
      return block("reference price unavailable or stale for maxNotional");
    if (!sanePositive(price))
      return block("order price unavailable for maxNotional");
    double mult;
    {
      std::lock_guard<std::mutex> lk(posMutex_); // only when notional is capped
      if (!multiplierKnownLocked(instrumentId))
        return block("instrument multiplier unavailable");
      auto supported = futuresMarginFormula_.find(instrumentId);
      if (supported == futuresMarginFormula_.end() || !supported->second)
        return block("instrument product class unsupported for maxNotional");
      mult = multiplierLocked(instrumentId);
    }
    const double valuationPrice = std::max(price, refPrice);
    const double notional = valuationPrice * static_cast<double>(volume) * mult;
    if (notional > maxNotional)
      return block("order notional exceeds maxNotional");
  }

  lastReason_ = nullptr;
  return {true, nullptr};
}

bool RiskEngine::allowUnsupportedRiskRequest() const {
  if (mode_.load(std::memory_order_acquire) != RiskMode::Normal)
    return false;
  std::lock_guard<std::mutex> lk(posMutex_);
  if ((sessionIdentityRequired_ && !sessionIdentityKnown_) ||
      ledgerUncertain_ || orderLedgerUncertain_ ||
      !unsupportedWorkingOrders_.empty() || localControlsEnabledLocked())
    return false;
  return true;
}

void RiskEngine::recordUnsupportedRiskMutation() {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(posMutex_);
  if (!unsupportedRiskMutationUsed_) {
    unsupportedRiskMutationUsed_ = true;
    revision_.fetch_add(1, std::memory_order_release);
  }
}

} // namespace ctp
