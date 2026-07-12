/*
 * mdspi.h - market-data SPI. Each CTP callback just pushes a binary record into
 * the EventChannel (no JS, no object building) on the CTP callback thread.
 *
 * Event codes are mirrored in src/market-data.ts (MD_BASE + n).
 */

#ifndef CTP_NATIVE_MDSPI_H
#define CTP_NATIVE_MDSPI_H

#include "ThostFtdcMdApi.h"
#include "arm.h"
#include "channel.h"
#include "snapshot.h"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ctp {

enum {
  MD_BASE = 0x1000,
  MD_FRONT_CONNECTED = MD_BASE + 1,
  MD_FRONT_DISCONNECTED = MD_BASE + 2,
  MD_HEARTBEAT_WARNING = MD_BASE + 3,
  MD_RSP_USER_LOGIN = MD_BASE + 4,
  MD_RSP_USER_LOGOUT = MD_BASE + 5,
  MD_RSP_QRY_MULTICAST_INSTRUMENT = MD_BASE + 6,
  MD_RSP_ERROR = MD_BASE + 7,
  MD_RSP_SUB_MARKET_DATA = MD_BASE + 8,
  MD_RSP_UNSUB_MARKET_DATA = MD_BASE + 9,
  MD_RSP_SUB_FOR_QUOTE = MD_BASE + 10,
  MD_RSP_UNSUB_FOR_QUOTE = MD_BASE + 11,
  MD_RTN_DEPTH_MARKET_DATA = MD_BASE + 12,
  MD_RTN_FOR_QUOTE = MD_BASE + 13,
};

class MdSpi : public CThostFtdcMdSpi {
public:
  explicit MdSpi(EventChannel *channel) : ch_(channel) {}
  ~MdSpi() {}

  // Feed depth ticks to a Trader's arm registry (or nullptr to detach).
  void setArmRegistry(std::shared_ptr<ArmRegistry> r) {
    std::lock_guard<std::mutex> lk(armRegistryMutex_);
    armReg_ = std::move(r);
  }
  // Latest-tick cache (LVC) updated on every depth tick (or nullptr to detach).
  void setSnapshotCache(SnapshotCache *c) { snap_.store(c, std::memory_order_release); }
  // Session-transition state is native because SimNow may echo requestId=0.
  // Only one login/logout may be outstanding; a JS timeout poisons transitions
  // until the next front callback so a late id-0 login cannot authenticate a
  // newer attempt.
  bool beginLogin(int requestId);
  bool beginLogout(int requestId);
  // Called by the JS idle timer. Returns true only when the timer won the
  // native state race and poisoned the still-pending transition. If false, a
  // final callback already changed state and its lossless row must be awaited.
  bool timeoutSessionTransition(int requestId);
  void abandonSessionTransition();
  // End the current authenticated MD epoch and reset transition ambiguity.
  // Reserved for concrete front transitions / close and immediate send failure.
  void invalidateSession();
  void noteSubscriptionRequest(const std::vector<std::string> &instrumentIds,
                               bool subscribe);

  void OnFrontConnected() override;
  void OnFrontDisconnected(int nReason) override;
  void OnHeartBeatWarning(int nTimeLapse) override;
  void OnRspUserLogin(CThostFtdcRspUserLoginField *p, CThostFtdcRspInfoField *e,
                      int id, bool last) override;
  void OnRspUserLogout(CThostFtdcUserLogoutField *p, CThostFtdcRspInfoField *e,
                       int id, bool last) override;
  void OnRspQryMulticastInstrument(CThostFtdcMulticastInstrumentField *p,
                                   CThostFtdcRspInfoField *e, int id,
                                   bool last) override;
  void OnRspError(CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspSubMarketData(CThostFtdcSpecificInstrumentField *p,
                          CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField *p,
                            CThostFtdcRspInfoField *e, int id,
                            bool last) override;
  void OnRspSubForQuoteRsp(CThostFtdcSpecificInstrumentField *p,
                           CThostFtdcRspInfoField *e, int id,
                           bool last) override;
  void OnRspUnSubForQuoteRsp(CThostFtdcSpecificInstrumentField *p,
                             CThostFtdcRspInfoField *e, int id,
                             bool last) override;
  void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField *p) override;
  void OnRtnForQuoteRsp(CThostFtdcForQuoteRspField *p) override;

private:
  enum class SessionState : uint8_t {
    Inactive,
    LoginPending,
    Authenticated,
    LogoutPending,
    Ambiguous,
  };
  void invalidateSessionLocked(); // sessionMutex_ held
  bool responseMatchesLocked(SessionState pendingState, int requestId) const;
  EventChannel *ch_;
  // Copy shared ownership before evaluating a tick. This makes replacing or
  // detaching a registry safe even when a callback already started.
  std::mutex armRegistryMutex_;
  std::shared_ptr<ArmRegistry> armReg_;
  std::atomic<SnapshotCache *> snap_{nullptr};
  // Linearizes login/disconnect/logout against tick-to-arm evaluation. Once an
  // invalidation callback returns, no earlier accepted tick can still fire.
  std::mutex sessionMutex_;
  // Protected by sessionMutex_.
  SessionState sessionState_ = SessionState::Inactive;
  int pendingSessionRequestId_ = 0;
  std::string tradingDay_;
  struct SubscriptionState {
    uint64_t latestGeneration = 0;
    bool desired = true;
    std::deque<uint64_t> subscribeGenerations;
    std::deque<uint64_t> unsubscribeGenerations;
  };
  uint64_t nextSubscriptionGeneration_ = 1;
  std::unordered_map<std::string, SubscriptionState> subscriptions_;
  bool subscriptionAllowsLocked(const std::string &instrumentId) const;
  void finishSubscriptionLocked(const std::string &instrumentId,
                                bool subscribe, bool success);
};

} // namespace ctp

#endif /* CTP_NATIVE_MDSPI_H */
