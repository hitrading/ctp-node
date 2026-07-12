/* AUTO-GENERATED. Do not edit. */
#include "traderreq.gen.h"
#include "ThostFtdcUserApiStruct.h"
#include "../native/order_capability.h"
#include "../native/risk.h"
#include <algorithm>
#include <cstring>

namespace ctp {

static bool validOffsetFlags(const char *flags, size_t capacity) {
  return capacity > 0 && flags[0] >= '0' && flags[0] <= '6' &&
         (capacity == 1 || flags[1] == '\0');
}

static bool hasOpenOffset(const char *flags, size_t capacity) {
  for (size_t i = 0; i < capacity && flags[i]; ++i)
    if (flags[i] == '0') return true;
  return false;
}

static bool validHedgeFlag(char flag) {
  return flag == '1' || flag == '2' || flag == '3' || flag == '5' ||
         flag == '6' || flag == '7';
}

static bool validSingleHedge(const char *flags, size_t capacity) {
  return capacity > 0 && validHedgeFlag(flags[0]) &&
         (capacity == 1 || flags[1] == '\0');
}

int traderReq(CThostFtdcTraderApi *api, int methodId, const void *bytes, int len,
              int requestId, RiskEngine *risk) {
  switch (methodId) {
  case M_ReqAuthenticate: {
    CThostFtdcReqAuthenticateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqAuthenticate(&f, requestId);
  }
  case M_ReqUserLogin: {
    CThostFtdcReqUserLoginField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) risk->beginSessionTransition(requestId, true, f.BrokerID, f.UserID);
    int sendRc = api->ReqUserLogin(&f, requestId);
    if (sendRc != 0 && risk) risk->finishSessionTransition(requestId);
    return sendRc;
  }
  case M_ReqUserLogout: {
    CThostFtdcUserLogoutField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) risk->beginSessionTransition(requestId, false, f.BrokerID, f.UserID);
    int sendRc = api->ReqUserLogout(&f, requestId);
    if (sendRc != 0 && risk) risk->finishSessionTransition(requestId);
    return sendRc;
  }
  case M_ReqUserPasswordUpdate: {
    CThostFtdcUserPasswordUpdateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqUserPasswordUpdate(&f, requestId);
  }
  case M_ReqTradingAccountPasswordUpdate: {
    CThostFtdcTradingAccountPasswordUpdateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqTradingAccountPasswordUpdate(&f, requestId);
  }
  case M_ReqUserAuthMethod: {
    CThostFtdcReqUserAuthMethodField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqUserAuthMethod(&f, requestId);
  }
  case M_ReqGenUserCaptcha: {
    CThostFtdcReqGenUserCaptchaField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqGenUserCaptcha(&f, requestId);
  }
  case M_ReqGenUserText: {
    CThostFtdcReqGenUserTextField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqGenUserText(&f, requestId);
  }
  case M_ReqUserLoginWithCaptcha: {
    CThostFtdcReqUserLoginWithCaptchaField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) risk->beginSessionTransition(requestId, true, f.BrokerID, f.UserID);
    int sendRc = api->ReqUserLoginWithCaptcha(&f, requestId);
    if (sendRc != 0 && risk) risk->finishSessionTransition(requestId);
    return sendRc;
  }
  case M_ReqUserLoginWithText: {
    CThostFtdcReqUserLoginWithTextField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) risk->beginSessionTransition(requestId, true, f.BrokerID, f.UserID);
    int sendRc = api->ReqUserLoginWithText(&f, requestId);
    if (sendRc != 0 && risk) risk->finishSessionTransition(requestId);
    return sendRc;
  }
  case M_ReqUserLoginWithOTP: {
    CThostFtdcReqUserLoginWithOTPField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) risk->beginSessionTransition(requestId, true, f.BrokerID, f.UserID);
    int sendRc = api->ReqUserLoginWithOTP(&f, requestId);
    if (sendRc != 0 && risk) risk->finishSessionTransition(requestId);
    return sendRc;
  }
  case M_ReqOrderInsert: {
    CThostFtdcInputOrderField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      OrdinaryOrderIdentity identity;
      if (!decodeOrdinaryOrder(f, identity)) return CTP_INVALID_ORDER_FIELDS;
      bool isOpen = hasOpenOffset(f.CombOffsetFlag, sizeof(f.CombOffsetFlag));
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSendForAccount(isOpen, identity.brokerId, identity.investorId, sendAllowed);
      if (!sendAllowed) return CTP_RISK_BLOCKED;
      RiskVerdict v = risk->check(identity.instrumentId, f.LimitPrice, risk->refPrice(identity.instrumentId), f.VolumeTotalOriginal, isOpen);
      if (!v.ok) return CTP_RISK_BLOCKED;
      if (isOpen) {
        OpenGate g = risk->tryReserveOpen(identity.orderRef, identity.instrumentId, f.Direction == '0', f.LimitPrice, f.VolumeTotalOriginal, f.CombHedgeFlag[0]);
        if (g == OpenGate::VolumeLimit) return CTP_POSITION_VOLUME_LIMIT;
        if (g == OpenGate::CostLimit) return CTP_POSITION_LIMIT;
        if (g == OpenGate::DuplicateOrderRef) return CTP_DUPLICATE_ORDER_REF;
        if (g == OpenGate::MetadataUnavailable) return CTP_RISK_METADATA_UNAVAILABLE;
        if (!risk->bindInsertRequest(requestId, identity.orderRef)) return CTP_RISK_METADATA_UNAVAILABLE;
      }
      if (isOpen && !risk->allowRate()) {
        risk->rollbackInsertRequest(requestId, identity.orderRef);
        return CTP_RATE_LIMITED;
      }
      if (!isOpen && !risk->allowCloseRate()) return CTP_RATE_LIMITED;
      int sendRc = api->ReqOrderInsert(&f, requestId);
      if (sendRc != 0 && isOpen) risk->rollbackInsertRequest(requestId, identity.orderRef); // request not accepted; safe to retry this ref
      return sendRc;
    }
    return api->ReqOrderInsert(&f, requestId);
  }
  case M_ReqParkedOrderInsert: {
    CThostFtdcParkedOrderField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqParkedOrderInsert(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqParkedOrderInsert(&f, requestId);
  }
  case M_ReqParkedOrderAction: {
    CThostFtdcParkedOrderActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk && f.ActionFlag != '0') {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqParkedOrderAction(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqParkedOrderAction(&f, requestId);
  }
  case M_ReqOrderAction: {
    CThostFtdcInputOrderActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk && f.ActionFlag != '0') {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqOrderAction(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqOrderAction(&f, requestId);
  }
  case M_ReqQryMaxOrderVolume: {
    CThostFtdcQryMaxOrderVolumeField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryMaxOrderVolume(&f, requestId);
  }
  case M_ReqSettlementInfoConfirm: {
    CThostFtdcSettlementInfoConfirmField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqSettlementInfoConfirm(&f, requestId);
  }
  case M_ReqRemoveParkedOrder: {
    CThostFtdcRemoveParkedOrderField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqRemoveParkedOrder(&f, requestId);
  }
  case M_ReqRemoveParkedOrderAction: {
    CThostFtdcRemoveParkedOrderActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqRemoveParkedOrderAction(&f, requestId);
  }
  case M_ReqExecOrderInsert: {
    CThostFtdcInputExecOrderField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqExecOrderInsert(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqExecOrderInsert(&f, requestId);
  }
  case M_ReqExecOrderAction: {
    CThostFtdcInputExecOrderActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk && f.ActionFlag != '0') {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqExecOrderAction(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqExecOrderAction(&f, requestId);
  }
  case M_ReqForQuoteInsert: {
    CThostFtdcInputForQuoteField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqForQuoteInsert(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqForQuoteInsert(&f, requestId);
  }
  case M_ReqQuoteInsert: {
    CThostFtdcInputQuoteField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqQuoteInsert(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqQuoteInsert(&f, requestId);
  }
  case M_ReqQuoteAction: {
    CThostFtdcInputQuoteActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk && f.ActionFlag != '0') {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqQuoteAction(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqQuoteAction(&f, requestId);
  }
  case M_ReqBatchOrderAction: {
    CThostFtdcInputBatchOrderActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqBatchOrderAction(&f, requestId);
  }
  case M_ReqOptionSelfCloseInsert: {
    CThostFtdcInputOptionSelfCloseField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqOptionSelfCloseInsert(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqOptionSelfCloseInsert(&f, requestId);
  }
  case M_ReqOptionSelfCloseAction: {
    CThostFtdcInputOptionSelfCloseActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk && f.ActionFlag != '0') {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqOptionSelfCloseAction(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqOptionSelfCloseAction(&f, requestId);
  }
  case M_ReqCombActionInsert: {
    CThostFtdcInputCombActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqCombActionInsert(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqCombActionInsert(&f, requestId);
  }
  case M_ReqQryOrder: {
    CThostFtdcQryOrderField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryOrder(&f, requestId);
  }
  case M_ReqQryTrade: {
    CThostFtdcQryTradeField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryTrade(&f, requestId);
  }
  case M_ReqQryInvestorPosition: {
    CThostFtdcQryInvestorPositionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorPosition(&f, requestId);
  }
  case M_ReqQryTradingAccount: {
    CThostFtdcQryTradingAccountField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryTradingAccount(&f, requestId);
  }
  case M_ReqQryInvestor: {
    CThostFtdcQryInvestorField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestor(&f, requestId);
  }
  case M_ReqQryTradingCode: {
    CThostFtdcQryTradingCodeField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryTradingCode(&f, requestId);
  }
  case M_ReqQryInstrumentMarginRate: {
    CThostFtdcQryInstrumentMarginRateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) risk->beginMarginScheduleQuery(requestId, false, f.InstrumentID, f.HedgeFlag, f.BrokerID, f.InvestorID);
    int sendRc = api->ReqQryInstrumentMarginRate(&f, requestId);
    if (sendRc != 0 && risk) risk->finishMarginScheduleQuery(requestId, false);
    return sendRc;
  }
  case M_ReqQryInstrumentCommissionRate: {
    CThostFtdcQryInstrumentCommissionRateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInstrumentCommissionRate(&f, requestId);
  }
  case M_ReqQryUserSession: {
    CThostFtdcQryUserSessionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryUserSession(&f, requestId);
  }
  case M_ReqQryExchange: {
    CThostFtdcQryExchangeField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryExchange(&f, requestId);
  }
  case M_ReqQryProduct: {
    CThostFtdcQryProductField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryProduct(&f, requestId);
  }
  case M_ReqQryInstrument: {
    CThostFtdcQryInstrumentField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) risk->invalidateInstrument(f.InstrumentID);
    return api->ReqQryInstrument(&f, requestId);
  }
  case M_ReqQryDepthMarketData: {
    CThostFtdcQryDepthMarketDataField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryDepthMarketData(&f, requestId);
  }
  case M_ReqQryTraderOffer: {
    CThostFtdcQryTraderOfferField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryTraderOffer(&f, requestId);
  }
  case M_ReqQrySettlementInfo: {
    CThostFtdcQrySettlementInfoField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySettlementInfo(&f, requestId);
  }
  case M_ReqQryTransferBank: {
    CThostFtdcQryTransferBankField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryTransferBank(&f, requestId);
  }
  case M_ReqQryInvestorPositionDetail: {
    CThostFtdcQryInvestorPositionDetailField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorPositionDetail(&f, requestId);
  }
  case M_ReqQryNotice: {
    CThostFtdcQryNoticeField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryNotice(&f, requestId);
  }
  case M_ReqQrySettlementInfoConfirm: {
    CThostFtdcQrySettlementInfoConfirmField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySettlementInfoConfirm(&f, requestId);
  }
  case M_ReqQryInvestorPositionCombineDetail: {
    CThostFtdcQryInvestorPositionCombineDetailField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorPositionCombineDetail(&f, requestId);
  }
  case M_ReqQryCFMMCTradingAccountKey: {
    CThostFtdcQryCFMMCTradingAccountKeyField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryCFMMCTradingAccountKey(&f, requestId);
  }
  case M_ReqQryEWarrantOffset: {
    CThostFtdcQryEWarrantOffsetField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryEWarrantOffset(&f, requestId);
  }
  case M_ReqQryInvestorProductGroupMargin: {
    CThostFtdcQryInvestorProductGroupMarginField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorProductGroupMargin(&f, requestId);
  }
  case M_ReqQryExchangeMarginRate: {
    CThostFtdcQryExchangeMarginRateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) risk->beginMarginScheduleQuery(requestId, true, f.InstrumentID, f.HedgeFlag, f.BrokerID, "");
    int sendRc = api->ReqQryExchangeMarginRate(&f, requestId);
    if (sendRc != 0 && risk) risk->finishMarginScheduleQuery(requestId, false);
    return sendRc;
  }
  case M_ReqQryExchangeMarginRateAdjust: {
    CThostFtdcQryExchangeMarginRateAdjustField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryExchangeMarginRateAdjust(&f, requestId);
  }
  case M_ReqQryExchangeRate: {
    CThostFtdcQryExchangeRateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryExchangeRate(&f, requestId);
  }
  case M_ReqQrySecAgentACIDMap: {
    CThostFtdcQrySecAgentACIDMapField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySecAgentACIDMap(&f, requestId);
  }
  case M_ReqQryProductExchRate: {
    CThostFtdcQryProductExchRateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryProductExchRate(&f, requestId);
  }
  case M_ReqQryProductGroup: {
    CThostFtdcQryProductGroupField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryProductGroup(&f, requestId);
  }
  case M_ReqQryMMInstrumentCommissionRate: {
    CThostFtdcQryMMInstrumentCommissionRateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryMMInstrumentCommissionRate(&f, requestId);
  }
  case M_ReqQryMMOptionInstrCommRate: {
    CThostFtdcQryMMOptionInstrCommRateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryMMOptionInstrCommRate(&f, requestId);
  }
  case M_ReqQryInstrumentOrderCommRate: {
    CThostFtdcQryInstrumentOrderCommRateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInstrumentOrderCommRate(&f, requestId);
  }
  case M_ReqQrySecAgentTradingAccount: {
    CThostFtdcQryTradingAccountField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySecAgentTradingAccount(&f, requestId);
  }
  case M_ReqQrySecAgentCheckMode: {
    CThostFtdcQrySecAgentCheckModeField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySecAgentCheckMode(&f, requestId);
  }
  case M_ReqQrySecAgentTradeInfo: {
    CThostFtdcQrySecAgentTradeInfoField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySecAgentTradeInfo(&f, requestId);
  }
  case M_ReqQryOptionInstrTradeCost: {
    CThostFtdcQryOptionInstrTradeCostField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryOptionInstrTradeCost(&f, requestId);
  }
  case M_ReqQryOptionInstrCommRate: {
    CThostFtdcQryOptionInstrCommRateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryOptionInstrCommRate(&f, requestId);
  }
  case M_ReqQryExecOrder: {
    CThostFtdcQryExecOrderField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryExecOrder(&f, requestId);
  }
  case M_ReqQryForQuote: {
    CThostFtdcQryForQuoteField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryForQuote(&f, requestId);
  }
  case M_ReqQryQuote: {
    CThostFtdcQryQuoteField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryQuote(&f, requestId);
  }
  case M_ReqQryOptionSelfClose: {
    CThostFtdcQryOptionSelfCloseField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryOptionSelfClose(&f, requestId);
  }
  case M_ReqQryInvestUnit: {
    CThostFtdcQryInvestUnitField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestUnit(&f, requestId);
  }
  case M_ReqQryCombInstrumentGuard: {
    CThostFtdcQryCombInstrumentGuardField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryCombInstrumentGuard(&f, requestId);
  }
  case M_ReqQryCombAction: {
    CThostFtdcQryCombActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryCombAction(&f, requestId);
  }
  case M_ReqQryTransferSerial: {
    CThostFtdcQryTransferSerialField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryTransferSerial(&f, requestId);
  }
  case M_ReqQryAccountregister: {
    CThostFtdcQryAccountregisterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryAccountregister(&f, requestId);
  }
  case M_ReqQryContractBank: {
    CThostFtdcQryContractBankField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryContractBank(&f, requestId);
  }
  case M_ReqQryParkedOrder: {
    CThostFtdcQryParkedOrderField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryParkedOrder(&f, requestId);
  }
  case M_ReqQryParkedOrderAction: {
    CThostFtdcQryParkedOrderActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryParkedOrderAction(&f, requestId);
  }
  case M_ReqQryTradingNotice: {
    CThostFtdcQryTradingNoticeField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryTradingNotice(&f, requestId);
  }
  case M_ReqQryBrokerTradingParams: {
    CThostFtdcQryBrokerTradingParamsField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryBrokerTradingParams(&f, requestId);
  }
  case M_ReqQryBrokerTradingAlgos: {
    CThostFtdcQryBrokerTradingAlgosField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryBrokerTradingAlgos(&f, requestId);
  }
  case M_ReqQueryCFMMCTradingAccountToken: {
    CThostFtdcQueryCFMMCTradingAccountTokenField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQueryCFMMCTradingAccountToken(&f, requestId);
  }
  case M_ReqFromBankToFutureByFuture: {
    CThostFtdcReqTransferField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqFromBankToFutureByFuture(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqFromBankToFutureByFuture(&f, requestId);
  }
  case M_ReqFromFutureToBankByFuture: {
    CThostFtdcReqTransferField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqFromFutureToBankByFuture(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqFromFutureToBankByFuture(&f, requestId);
  }
  case M_ReqQueryBankAccountMoneyByFuture: {
    CThostFtdcReqQueryAccountField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQueryBankAccountMoneyByFuture(&f, requestId);
  }
  case M_ReqQryClassifiedInstrument: {
    CThostFtdcQryClassifiedInstrumentField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryClassifiedInstrument(&f, requestId);
  }
  case M_ReqQryCombPromotionParam: {
    CThostFtdcQryCombPromotionParamField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryCombPromotionParam(&f, requestId);
  }
  case M_ReqQryRiskSettleInvstPosition: {
    CThostFtdcQryRiskSettleInvstPositionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryRiskSettleInvstPosition(&f, requestId);
  }
  case M_ReqQryRiskSettleProductStatus: {
    CThostFtdcQryRiskSettleProductStatusField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryRiskSettleProductStatus(&f, requestId);
  }
  case M_ReqQrySPBMFutureParameter: {
    CThostFtdcQrySPBMFutureParameterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySPBMFutureParameter(&f, requestId);
  }
  case M_ReqQrySPBMOptionParameter: {
    CThostFtdcQrySPBMOptionParameterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySPBMOptionParameter(&f, requestId);
  }
  case M_ReqQrySPBMIntraParameter: {
    CThostFtdcQrySPBMIntraParameterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySPBMIntraParameter(&f, requestId);
  }
  case M_ReqQrySPBMInterParameter: {
    CThostFtdcQrySPBMInterParameterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySPBMInterParameter(&f, requestId);
  }
  case M_ReqQrySPBMPortfDefinition: {
    CThostFtdcQrySPBMPortfDefinitionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySPBMPortfDefinition(&f, requestId);
  }
  case M_ReqQrySPBMInvestorPortfDef: {
    CThostFtdcQrySPBMInvestorPortfDefField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySPBMInvestorPortfDef(&f, requestId);
  }
  case M_ReqQryInvestorPortfMarginRatio: {
    CThostFtdcQryInvestorPortfMarginRatioField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorPortfMarginRatio(&f, requestId);
  }
  case M_ReqQryInvestorProdSPBMDetail: {
    CThostFtdcQryInvestorProdSPBMDetailField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorProdSPBMDetail(&f, requestId);
  }
  case M_ReqQryInvestorCommoditySPMMMargin: {
    CThostFtdcQryInvestorCommoditySPMMMarginField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorCommoditySPMMMargin(&f, requestId);
  }
  case M_ReqQryInvestorCommodityGroupSPMMMargin: {
    CThostFtdcQryInvestorCommodityGroupSPMMMarginField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorCommodityGroupSPMMMargin(&f, requestId);
  }
  case M_ReqQrySPMMInstParam: {
    CThostFtdcQrySPMMInstParamField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySPMMInstParam(&f, requestId);
  }
  case M_ReqQrySPMMProductParam: {
    CThostFtdcQrySPMMProductParamField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySPMMProductParam(&f, requestId);
  }
  case M_ReqQrySPBMAddOnInterParameter: {
    CThostFtdcQrySPBMAddOnInterParameterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySPBMAddOnInterParameter(&f, requestId);
  }
  case M_ReqQryRCAMSCombProductInfo: {
    CThostFtdcQryRCAMSCombProductInfoField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryRCAMSCombProductInfo(&f, requestId);
  }
  case M_ReqQryRCAMSInstrParameter: {
    CThostFtdcQryRCAMSInstrParameterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryRCAMSInstrParameter(&f, requestId);
  }
  case M_ReqQryRCAMSIntraParameter: {
    CThostFtdcQryRCAMSIntraParameterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryRCAMSIntraParameter(&f, requestId);
  }
  case M_ReqQryRCAMSInterParameter: {
    CThostFtdcQryRCAMSInterParameterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryRCAMSInterParameter(&f, requestId);
  }
  case M_ReqQryRCAMSShortOptAdjustParam: {
    CThostFtdcQryRCAMSShortOptAdjustParamField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryRCAMSShortOptAdjustParam(&f, requestId);
  }
  case M_ReqQryRCAMSInvestorCombPosition: {
    CThostFtdcQryRCAMSInvestorCombPositionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryRCAMSInvestorCombPosition(&f, requestId);
  }
  case M_ReqQryInvestorProdRCAMSMargin: {
    CThostFtdcQryInvestorProdRCAMSMarginField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorProdRCAMSMargin(&f, requestId);
  }
  case M_ReqQryRULEInstrParameter: {
    CThostFtdcQryRULEInstrParameterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryRULEInstrParameter(&f, requestId);
  }
  case M_ReqQryRULEIntraParameter: {
    CThostFtdcQryRULEIntraParameterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryRULEIntraParameter(&f, requestId);
  }
  case M_ReqQryRULEInterParameter: {
    CThostFtdcQryRULEInterParameterField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryRULEInterParameter(&f, requestId);
  }
  case M_ReqQryInvestorProdRULEMargin: {
    CThostFtdcQryInvestorProdRULEMarginField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorProdRULEMargin(&f, requestId);
  }
  case M_ReqQryInvestorPortfSetting: {
    CThostFtdcQryInvestorPortfSettingField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorPortfSetting(&f, requestId);
  }
  case M_ReqQryInvestorInfoCommRec: {
    CThostFtdcQryInvestorInfoCommRecField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInvestorInfoCommRec(&f, requestId);
  }
  case M_ReqQryCombLeg: {
    CThostFtdcQryCombLegField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryCombLeg(&f, requestId);
  }
  case M_ReqOffsetSetting: {
    CThostFtdcInputOffsetSettingField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqOffsetSetting(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqOffsetSetting(&f, requestId);
  }
  case M_ReqCancelOffsetSetting: {
    CThostFtdcInputOffsetSettingField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqCancelOffsetSetting(&f, requestId);
  }
  case M_ReqQryOffsetSetting: {
    CThostFtdcQryOffsetSettingField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryOffsetSetting(&f, requestId);
  }
  case M_ReqGenSMSCode: {
    CThostFtdcReqGenSMSCodeField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqGenSMSCode(&f, requestId);
  }
  case M_ReqSpdApply: {
    CThostFtdcInputSpdApplyField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqSpdApply(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqSpdApply(&f, requestId);
  }
  case M_ReqSpdApplyAction: {
    CThostFtdcInputSpdApplyActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqSpdApplyAction(&f, requestId);
  }
  case M_ReqQrySpdApply: {
    CThostFtdcQrySpdApplyField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQrySpdApply(&f, requestId);
  }
  case M_ReqHedgeCfm: {
    CThostFtdcInputHedgeCfmField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      bool sendAllowed = false;
      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);
      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;
      int sendRc = api->ReqHedgeCfm(&f, requestId);
      if (sendRc == 0) risk->recordUnsupportedRiskMutation();
      return sendRc;
    }
    return api->ReqHedgeCfm(&f, requestId);
  }
  case M_ReqHedgeCfmAction: {
    CThostFtdcInputHedgeCfmActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqHedgeCfmAction(&f, requestId);
  }
  case M_ReqQryHedgeCfm: {
    CThostFtdcQryHedgeCfmField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryHedgeCfm(&f, requestId);
  }
  default:
    return -1;
  }
}

} // namespace ctp
