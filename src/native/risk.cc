/*
 * risk.cc - see risk.h
 */

#include "risk.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace ctp {

// A price / multiplier is usable in the notional math only if it is finite,
// strictly positive, and below the CTP "unset" sentinel band. DBL_MAX (~1.8e308,
// what CTP puts in untouched price fields before the first trade) IS finite, so
// it slips past isfinite alone; 1e300 is far above any real value and far below
// DBL_MAX. Centralized so every double entering the math rejects the sentinel
// uniformly.
static inline bool sanePositive(double x) {
  return std::isfinite(x) && x > 0.0 && x < 1e300;
}

// Broker, investor and instrument identifiers cannot be padded.
static bool validIdentity(const std::string &s) {
  return !s.empty() && static_cast<unsigned char>(s.front()) > ' ' &&
         static_cast<unsigned char>(s.back()) > ' ' &&
         s.find('\0') == std::string::npos;
}

static inline double positiveLimitOrDisabled(double x) {
  // Exactly zero disables a limit. Negative/NaN/Inf is never a valid limit:
  // make it fail closed for native callers that bypass the TS guards.
  if (!std::isfinite(x) || x < 0.0)
    return std::numeric_limits<double>::min();
  return x > 0.0 ? x : 0.0;
}

void RateLimiter::configure(double ratePerSec, double burst) {
  std::lock_guard<std::mutex> lk(m_);
  if (!std::isfinite(ratePerSec) || !std::isfinite(burst) ||
      ratePerSec < 0.0 || burst < 0.0) {
    rate_ = std::numeric_limits<double>::min();
    burst_ = 1.0;
    tokens_ = 0.0; // malformed native config fails closed
    last_ = std::chrono::steady_clock::now();
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
}

bool RateLimiter::tryAcquire() {
  std::lock_guard<std::mutex> lk(m_);
  if (rate_ <= 0.0)
    return true; // disabled

  const auto now = std::chrono::steady_clock::now();

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
      cfg.closeOrderBurst < 0.0 || !std::isfinite(cfg.maxReferenceAgeMs) ||
      cfg.maxReferenceAgeMs <= 0.0;
  invalidConfig_.store(invalid, std::memory_order_release);
  maxOrderVolume_.store(cfg.maxOrderVolume > 0 ? cfg.maxOrderVolume : 0,
                        std::memory_order_relaxed);
  maxPriceDeviation_.store(positiveLimitOrDisabled(cfg.maxPriceDeviation),
                           std::memory_order_relaxed);
  maxNotional_.store(positiveLimitOrDisabled(cfg.maxNotional),
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
    std::lock_guard<std::mutex> state(stateMutex_);
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
  // Read the reference from the live MD snapshot (in C++). Copy the shared_ptr
  // under our lock, then query outside it (the cache has its own mutex); no
  // nested locking, and a concurrent detach can't free it mid-read. A
  // non-finite / DBL_MAX "unset" last price is rejected here (not in the cache)
  // so a sentinel reference can't make |price-ref|/ref == 1.0 and falsely block
  // every order. An enabled reference rule fails closed on a 0 reference.
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

void RiskEngine::resetInstrumentQueryLocked() {
  instrumentQueryId_ = 0;
  instrumentQueryUnreadable_ = false;
  stagedInstruments_.clear();
}

void RiskEngine::beginInstrumentQuery(int requestId,
                                      const std::string &instrumentId) {
  std::lock_guard<std::mutex> lk(stateMutex_);
  if (instrumentId.empty())
    instruments_.clear();
  else
    instruments_.erase(instrumentId);
  resetInstrumentQueryLocked();
  instrumentQueryId_ = requestId > 0 ? requestId : 0;
}

void RiskEngine::stageInstrument(int requestId,
                                 const std::string &instrumentId, double mult,
                                 char productClass) {
  std::lock_guard<std::mutex> lk(stateMutex_);
  if (requestId <= 0 || requestId != instrumentQueryId_ ||
      instrumentId.empty())
    return;
  // Futures and TAS use price * volume * multiplier. Options, combinations and
  // spot products require product-specific fields/formulas.
  stagedInstruments_[instrumentId] =
      sanePositive(mult)
          ? InstrumentMeta{mult, productClass == '1' || productClass == '7'}
          : InstrumentMeta{};
}

void RiskEngine::stageUnreadableInstrument(int requestId) {
  std::lock_guard<std::mutex> lk(stateMutex_);
  if (requestId > 0 && requestId == instrumentQueryId_)
    instrumentQueryUnreadable_ = true;
}

void RiskEngine::finishInstrumentQuery(int requestId, bool succeeded) {
  std::lock_guard<std::mutex> lk(stateMutex_);
  if (requestId <= 0 || requestId != instrumentQueryId_)
    return;
  if (succeeded && instrumentQueryUnreadable_) {
    instruments_.clear();
  } else if (succeeded) {
    for (const auto &staged : stagedInstruments_) {
      if (staged.second.multiplier > 0.0)
        instruments_[staged.first] = staged.second;
      else
        instruments_.erase(staged.first);
    }
  }
  resetInstrumentQueryLocked();
}

void RiskEngine::abandonInstrumentQuery() {
  std::lock_guard<std::mutex> lk(stateMutex_);
  resetInstrumentQueryLocked();
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
  if (!validIdentity(brokerId) || !validIdentity(investorId))
    return AccountScope::Ambiguous;
  return brokerId == myBrokerId_ && investorId == myInvestorId_
             ? AccountScope::Current
             : AccountScope::Foreign;
}

void RiskEngine::invalidateSession() {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(stateMutex_);
  sessionIdentityRequired_ = true;
  sessionIdentityKnown_ = false;
  loginSessionKnown_ = false;
  pendingSessionRequestId_ = 0;
  pendingSessionExpectsLogin_ = false;
  pendingSessionBrokerId_.clear();
  pendingSessionUserId_.clear();
}

void RiskEngine::beginSessionTransition(int requestId, bool expectsLogin,
                                        const std::string &brokerId,
                                        const std::string &userId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(stateMutex_);
  sessionIdentityRequired_ = true;
  sessionIdentityKnown_ = false;
  loginSessionKnown_ = false;
  pendingSessionRequestId_ = requestId > 0 ? requestId : 0;
  pendingSessionExpectsLogin_ = expectsLogin && requestId > 0;
  pendingSessionBrokerId_ = brokerId;
  pendingSessionUserId_ = userId;
}

bool RiskEngine::acceptLoginSession(int requestId, int frontId, int sessionId,
                                    const std::string &brokerId,
                                    const std::string &userId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(stateMutex_);
  if (requestId <= 0 || requestId != pendingSessionRequestId_ ||
      !pendingSessionExpectsLogin_ || brokerId.empty() || userId.empty() ||
      frontId <= 0 || sessionId == 0 ||
      (!pendingSessionBrokerId_.empty() &&
       brokerId != pendingSessionBrokerId_) ||
      (!pendingSessionUserId_.empty() && userId != pendingSessionUserId_))
    return false;
  myFrontId_ = frontId;
  mySessionId_ = sessionId;
  myBrokerId_ = brokerId;
  sessionIdentityRequired_ = true;
  sessionIdentityKnown_ = false;
  loginSessionKnown_ = true;
  pendingSessionRequestId_ = 0;
  pendingSessionExpectsLogin_ = false;
  pendingSessionBrokerId_.clear();
  pendingSessionUserId_.clear();
  return true;
}

bool RiskEngine::confirmSessionAccount(
    int frontId, int sessionId, const std::string &brokerId,
    const std::string &investorId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(stateMutex_);
  if (!loginSessionKnown_ || frontId <= 0 || sessionId == 0 ||
      frontId != myFrontId_ || sessionId != mySessionId_ || brokerId.empty() ||
      investorId.empty() || brokerId != myBrokerId_)
    return false;
  myInvestorId_ = investorId;
  sessionIdentityRequired_ = true;
  sessionIdentityKnown_ = true;
  return true;
}

bool RiskEngine::acceptsOrderAccount(const std::string &brokerId,
                                     const std::string &investorId) const {
  std::lock_guard<std::mutex> lk(stateMutex_);
  return accountScopeLocked(brokerId, investorId) == AccountScope::Current;
}

void RiskEngine::finishSessionTransition(int requestId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(stateMutex_);
  if (requestId <= 0 || requestId != pendingSessionRequestId_)
    return;
  pendingSessionRequestId_ = 0;
  pendingSessionExpectsLogin_ = false;
  pendingSessionBrokerId_.clear();
  pendingSessionUserId_.clear();
}

bool RiskEngine::timeoutSessionTransition(int requestId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(stateMutex_);
  if (requestId <= 0 || requestId != pendingSessionRequestId_)
    return false;
  pendingSessionRequestId_ = 0;
  pendingSessionExpectsLogin_ = false;
  pendingSessionBrokerId_.clear();
  pendingSessionUserId_.clear();
  sessionIdentityKnown_ = false;
  loginSessionKnown_ = false;
  return true;
}

void RiskEngine::setSession(int frontId, int sessionId,
                            const std::string &brokerId,
                            const std::string &investorId) {
  std::lock_guard<RiskSendMutex> send(sendModeMutex_);
  std::lock_guard<std::mutex> lk(stateMutex_);
  const bool concreteIdentity = !sessionIdentityRequired_ ||
                                (!brokerId.empty() && !investorId.empty());
  myFrontId_ = frontId;
  mySessionId_ = sessionId;
  myBrokerId_ = brokerId;
  myInvestorId_ = investorId;
  sessionIdentityRequired_ = true;
  sessionIdentityKnown_ = concreteIdentity;
  loginSessionKnown_ = concreteIdentity;
  pendingSessionRequestId_ = 0;
  pendingSessionExpectsLogin_ = false;
  pendingSessionBrokerId_.clear();
  pendingSessionUserId_.clear();
}

bool RiskEngine::localControlsEnabled() const {
  return invalidConfig_.load(std::memory_order_acquire) ||
         maxOrderVolume_.load(std::memory_order_relaxed) > 0 ||
         maxPriceDeviation_.load(std::memory_order_relaxed) > 0.0 ||
         maxNotional_.load(std::memory_order_relaxed) > 0.0 ||
         limiter_.enabled() || closeLimiter_.enabled();
}

bool RiskEngine::allowUnsupportedRiskRequest() const {
  // Each refusal records its cause (lastBlockReason) so the -10006 rejection
  // can say which condition closed the path.
  if (mode_.load(std::memory_order_acquire) != RiskMode::Normal) {
    block("unsupported requests blocked outside NORMAL mode");
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (sessionIdentityRequired_ && !sessionIdentityKnown_) {
      block("session account not confirmed");
      return false;
    }
  }
  if (localControlsEnabled()) {
    block("unsupported requests blocked while local risk controls are configured");
    return false;
  }
  lastReason_ = nullptr;
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

  // A market/any-price order legitimately has price 0 (finite), so isfinite
  // allows it; NaN/Inf/negative never reach the vendor.
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
      std::lock_guard<std::mutex> lk(stateMutex_); // only when notional is capped
      auto known = instruments_.find(instrumentId);
      if (known == instruments_.end())
        return block("instrument multiplier unavailable");
      if (!known->second.futuresNotional)
        return block("instrument product class unsupported for maxNotional");
      mult = known->second.multiplier;
    }
    const double valuationPrice = std::max(price, refPrice);
    const double notional = valuationPrice * static_cast<double>(volume) * mult;
    if (notional > maxNotional)
      return block("order notional exceeds maxNotional");
  }

  lastReason_ = nullptr;
  return {true, nullptr};
}

} // namespace ctp
