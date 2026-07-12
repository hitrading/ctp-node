/*
 * mdspi.cc - see mdspi.h
 */

#include "mdspi.h"
#include "ctp_field.h"

#include <cstdio>

#include "../generated/structids.gen.h"

namespace ctp {

static int eid(CThostFtdcRspInfoField *e) { return e ? e->ErrorID : 0; }
static const char *emsg(CThostFtdcRspInfoField *e) {
  return (e && e->ErrorID) ? e->ErrorMsg : "";
}

void MdSpi::invalidateSessionLocked() {
  tradingDay_.clear();
  pendingSessionRequestId_ = 0;
  sessionState_ = SessionState::Inactive;
  subscriptions_.clear();
  SnapshotCache *sc = snap_.load(std::memory_order_acquire);
  if (sc)
    sc->invalidate();
}

void MdSpi::noteSubscriptionRequest(
    const std::vector<std::string> &instrumentIds, bool subscribe) {
  std::lock_guard<std::mutex> sessionLock(sessionMutex_);
  uint64_t generation = nextSubscriptionGeneration_++;
  if (generation == 0)
    generation = nextSubscriptionGeneration_++;
  for (const std::string &instrumentId : instrumentIds) {
    SubscriptionState &state = subscriptions_[instrumentId];
    state.latestGeneration = generation;
    state.desired = subscribe;
    auto &queue = subscribe ? state.subscribeGenerations
                            : state.unsubscribeGenerations;
    queue.push_back(generation);
  }
}

bool MdSpi::subscriptionAllowsLocked(
    const std::string &instrumentId) const {
  auto it = subscriptions_.find(instrumentId);
  return it == subscriptions_.end() || it->second.desired;
}

void MdSpi::finishSubscriptionLocked(const std::string &instrumentId,
                                     bool subscribe, bool success) {
  auto state = subscriptions_.find(instrumentId);
  if (state == subscriptions_.end())
    return;
  auto &queue = subscribe ? state->second.subscribeGenerations
                          : state->second.unsubscribeGenerations;
  if (queue.empty())
    return;
  const uint64_t generation = queue.front();
  queue.pop_front();
  if (generation != state->second.latestGeneration)
    return; // a newer opposite/same-kind request owns the final intent
  state->second.desired = success ? subscribe : !subscribe;
  SnapshotCache *sc = snap_.load(std::memory_order_acquire);
  if (!state->second.desired && sc)
    sc->erase(instrumentId);
}

void MdSpi::invalidateSession() {
  std::lock_guard<std::mutex> sessionLock(sessionMutex_);
  invalidateSessionLocked();
}

bool MdSpi::beginLogin(int requestId) {
  std::lock_guard<std::mutex> sessionLock(sessionMutex_);
  if (requestId <= 0 || sessionState_ == SessionState::LoginPending ||
      sessionState_ == SessionState::LogoutPending ||
      sessionState_ == SessionState::Ambiguous)
    return false;
  invalidateSessionLocked();
  sessionState_ = SessionState::LoginPending;
  pendingSessionRequestId_ = requestId;
  return true;
}

bool MdSpi::beginLogout(int requestId) {
  std::lock_guard<std::mutex> sessionLock(sessionMutex_);
  if (requestId <= 0 || sessionState_ == SessionState::LoginPending ||
      sessionState_ == SessionState::LogoutPending ||
      sessionState_ == SessionState::Ambiguous)
    return false;
  invalidateSessionLocked();
  sessionState_ = SessionState::LogoutPending;
  pendingSessionRequestId_ = requestId;
  return true;
}

void MdSpi::abandonSessionTransition() {
  std::lock_guard<std::mutex> sessionLock(sessionMutex_);
  if (sessionState_ != SessionState::LoginPending &&
      sessionState_ != SessionState::LogoutPending)
    return;
  tradingDay_.clear();
  pendingSessionRequestId_ = 0;
  sessionState_ = SessionState::Ambiguous;
  SnapshotCache *sc = snap_.load(std::memory_order_acquire);
  if (sc)
    sc->invalidate();
}

bool MdSpi::timeoutSessionTransition(int requestId) {
  std::lock_guard<std::mutex> sessionLock(sessionMutex_);
  const bool pending = sessionState_ == SessionState::LoginPending ||
                       sessionState_ == SessionState::LogoutPending;
  if (!pending)
    return false; // final callback/front transition already won the mutex
  if (requestId <= 0 || requestId != pendingSessionRequestId_)
    return true; // not this native transition; do not mutate somebody else's
  tradingDay_.clear();
  pendingSessionRequestId_ = 0;
  sessionState_ = SessionState::Ambiguous;
  SnapshotCache *sc = snap_.load(std::memory_order_acquire);
  if (sc)
    sc->invalidate();
  return true;
}

bool MdSpi::responseMatchesLocked(SessionState pendingState,
                                  int requestId) const {
  return sessionState_ == pendingState &&
         (requestId == pendingSessionRequestId_ || requestId == 0);
}

void MdSpi::OnFrontConnected() {
  invalidateSession(); // connected is not authenticated; await successful login
  ch_->push(MD_FRONT_CONNECTED, 0, -1, 0, "", -1, nullptr, 0);
}

void MdSpi::OnFrontDisconnected(int nReason) {
  invalidateSession();
  int r = nReason;
  ch_->push(MD_FRONT_DISCONNECTED, 0, -1, 0, "", -2, &r, sizeof(r));
}

void MdSpi::OnHeartBeatWarning(int nTimeLapse) {
  int t = nTimeLapse;
  ch_->push(MD_HEARTBEAT_WARNING, 0, -1, 0, "", -2, &t, sizeof(t));
}

void MdSpi::OnRspUserLogin(CThostFtdcRspUserLoginField *p,
                           CThostFtdcRspInfoField *e, int id, bool last) {
  // Login responses are normally a single final row. Require the final success
  // carrying session data before exposing quotes; an empty/error final response
  // leaves the cache fail-closed.
  if (last) {
    std::lock_guard<std::mutex> sessionLock(sessionMutex_);
    if (responseMatchesLocked(SessionState::LoginPending, id)) {
      pendingSessionRequestId_ = 0;
      std::string tradingDay;
      // An MD login carries no FrontID/SessionID (those are the trade front's
      // order-routing identity and are commonly 0 on the market-data channel). A
      // successful final login row with a well-formed TradingDay is the correct
      // activation signal; requiring a trade session id here left the snapshot
      // cache permanently inactive on real MD logins.
      if (p && eid(e) == 0 && boundedCtpField(p->TradingDay, tradingDay)) {
        tradingDay_ = std::move(tradingDay);
        sessionState_ = SessionState::Authenticated;
        SnapshotCache *sc = snap_.load(std::memory_order_acquire);
        if (sc)
          sc->activate();
      } else {
        invalidateSessionLocked();
      }
    } else if (sessionState_ == SessionState::Ambiguous) {
      // The timed-out request is the only possible outstanding transition.
      // Its final callback resolves the ambiguity, but is never allowed to
      // authenticate after the JS caller has already abandoned that epoch.
      invalidateSessionLocked();
    }
  }
  ch_->push(MD_RSP_USER_LOGIN, id, last ? 1 : 0, eid(e), emsg(e),
            p ? SID_RspUserLogin : -1, p, p ? (int)sizeof(*p) : 0);
}

void MdSpi::OnRspUserLogout(CThostFtdcUserLogoutField *p,
                            CThostFtdcRspInfoField *e, int id, bool last) {
  if (last) {
    std::lock_guard<std::mutex> sessionLock(sessionMutex_);
    if (eid(e) == 0) {
      // A successful logout is always conservative, even if it was initiated
      // outside this wrapper or is a late callback from an older request.
      invalidateSessionLocked();
    } else if (responseMatchesLocked(SessionState::LogoutPending, id) ||
               sessionState_ == SessionState::Ambiguous) {
      invalidateSessionLocked();
    }
  }
  ch_->push(MD_RSP_USER_LOGOUT, id, last ? 1 : 0, eid(e), emsg(e),
            p ? SID_UserLogout : -1, p, p ? (int)sizeof(*p) : 0);
}

void MdSpi::OnRspQryMulticastInstrument(CThostFtdcMulticastInstrumentField *p,
                                        CThostFtdcRspInfoField *e, int id,
                                        bool last) {
  ch_->push(MD_RSP_QRY_MULTICAST_INSTRUMENT, id, last ? 1 : 0, eid(e), emsg(e),
            p ? SID_MulticastInstrument : -1, p, p ? (int)sizeof(*p) : 0);
}

void MdSpi::OnRspError(CThostFtdcRspInfoField *e, int id, bool last) {
  if (id > 0 && eid(e) != 0) {
    std::lock_guard<std::mutex> sessionLock(sessionMutex_);
    if (responseMatchesLocked(SessionState::LoginPending, id) ||
        responseMatchesLocked(SessionState::LogoutPending, id))
      invalidateSessionLocked();
  }
  ch_->push(MD_RSP_ERROR, id, last ? 1 : 0, eid(e), emsg(e), -1, nullptr, 0);
}

void MdSpi::OnRspSubMarketData(CThostFtdcSpecificInstrumentField *p,
                               CThostFtdcRspInfoField *e, int id, bool last) {
  if (p) {
    std::string instrumentId;
    if (boundedCtpField(p->InstrumentID, instrumentId)) {
      std::lock_guard<std::mutex> sessionLock(sessionMutex_);
      finishSubscriptionLocked(instrumentId, true, eid(e) == 0);
    }
  }
  ch_->push(MD_RSP_SUB_MARKET_DATA, id, last ? 1 : 0, eid(e), emsg(e),
            p ? SID_SpecificInstrument : -1, p, p ? (int)sizeof(*p) : 0);
}

void MdSpi::OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField *p,
                                 CThostFtdcRspInfoField *e, int id, bool last) {
  if (p) {
    std::string instrumentId;
    if (boundedCtpField(p->InstrumentID, instrumentId)) {
      std::lock_guard<std::mutex> sessionLock(sessionMutex_);
      finishSubscriptionLocked(instrumentId, false, eid(e) == 0);
    }
  }
  ch_->push(MD_RSP_UNSUB_MARKET_DATA, id, last ? 1 : 0, eid(e), emsg(e),
            p ? SID_SpecificInstrument : -1, p, p ? (int)sizeof(*p) : 0);
}

void MdSpi::OnRspSubForQuoteRsp(CThostFtdcSpecificInstrumentField *p,
                                CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(MD_RSP_SUB_FOR_QUOTE, id, last ? 1 : 0, eid(e), emsg(e),
            p ? SID_SpecificInstrument : -1, p, p ? (int)sizeof(*p) : 0);
}

void MdSpi::OnRspUnSubForQuoteRsp(CThostFtdcSpecificInstrumentField *p,
                                  CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(MD_RSP_UNSUB_FOR_QUOTE, id, last ? 1 : 0, eid(e), emsg(e),
            p ? SID_SpecificInstrument : -1, p, p ? (int)sizeof(*p) : 0);
}

void MdSpi::OnRtnDepthMarketData(CThostFtdcDepthMarketDataField *p) {
  // Update the latest-tick cache, evaluate armed triggers (lowest tick-to-order
  // latency), then publish to the ring.
  if (p) {
    // The callback buffer belongs to the vendor API. Fill missing TradingDay on
    // our own copy rather than mutating memory the provider may reuse.
    CThostFtdcDepthMarketDataField tick = *p;
    std::string instrumentId;
    std::string tickTradingDay;
    const bool fieldsValid = boundedCtpField(tick.InstrumentID, instrumentId) &&
                             !instrumentId.empty() &&
                             boundedCtpField(tick.TradingDay, tickTradingDay);
    {
      // Linearize the complete cache-update -> arm-evaluation operation against
      // lifecycle invalidation. SnapshotCache's own mutex is released before
      // onTick because armed risk checks may read the same snapshot source.
      std::lock_guard<std::mutex> sessionLock(sessionMutex_);
      if (fieldsValid && tickTradingDay.empty()) {
        if (!tradingDay_.empty())
          std::snprintf(tick.TradingDay, sizeof(tick.TradingDay), "%s",
                        tradingDay_.c_str());
      }
      SnapshotCache *sc = snap_.load(std::memory_order_acquire);
      const bool accepted = fieldsValid &&
                            subscriptionAllowsLocked(instrumentId) && sc &&
                            sc->update(instrumentId, tick);
      // The cache's authenticated epoch is also the mandate clock. A callback
      // racing after disconnect/logout (or arriving before login) remains
      // visible as a protocol event, but must never fire an armed order.
      if (accepted) {
        std::shared_ptr<ArmRegistry> ar;
        {
          std::lock_guard<std::mutex> lk(armRegistryMutex_);
          ar = armReg_;
        }
        if (ar)
          ar->onTick(instrumentId.c_str(), tick.BidPrice1, tick.AskPrice1);
      }
    }
    ch_->push(MD_RTN_DEPTH_MARKET_DATA, 0, -1, 0, "", SID_DepthMarketData,
              &tick, (int)sizeof(tick), true);
    return;
  }
  ch_->push(MD_RTN_DEPTH_MARKET_DATA, 0, -1, 0, "", SID_DepthMarketData,
            nullptr, 0, true);
}

void MdSpi::OnRtnForQuoteRsp(CThostFtdcForQuoteRspField *p) {
  ch_->push(MD_RTN_FOR_QUOTE, 0, -1, 0, "", SID_ForQuoteRsp, p,
            p ? (int)sizeof(*p) : 0);
}

} // namespace ctp
