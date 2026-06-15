/* AUTO-GENERATED. Do not edit. */
#include "traderreq.gen.h"
#include "ThostFtdcUserApiStruct.h"
#include "../native/risk.h"
#include <algorithm>
#include <cstring>

namespace ctp {

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
    return api->ReqUserLogin(&f, requestId);
  }
  case M_ReqUserLogout: {
    CThostFtdcUserLogoutField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqUserLogout(&f, requestId);
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
    return api->ReqUserLoginWithCaptcha(&f, requestId);
  }
  case M_ReqUserLoginWithText: {
    CThostFtdcReqUserLoginWithTextField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqUserLoginWithText(&f, requestId);
  }
  case M_ReqUserLoginWithOTP: {
    CThostFtdcReqUserLoginWithOTPField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqUserLoginWithOTP(&f, requestId);
  }
  case M_ReqOrderInsert: {
    CThostFtdcInputOrderField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    if (risk) {
      RiskVerdict v = risk->check(f.LimitPrice, risk->refPrice(f.InstrumentID), f.VolumeTotalOriginal);
      if (!v.ok) return CTP_RISK_BLOCKED;
      if (f.CombOffsetFlag[0] == '0' &&
          !risk->allowOpen(f.InstrumentID, f.LimitPrice, f.VolumeTotalOriginal))
        return CTP_POSITION_LIMIT;
      if (!risk->allowRate()) return CTP_RATE_LIMITED;
    }
    return api->ReqOrderInsert(&f, requestId);
  }
  case M_ReqParkedOrderInsert: {
    CThostFtdcParkedOrderField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqParkedOrderInsert(&f, requestId);
  }
  case M_ReqParkedOrderAction: {
    CThostFtdcParkedOrderActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqParkedOrderAction(&f, requestId);
  }
  case M_ReqOrderAction: {
    CThostFtdcInputOrderActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
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
    return api->ReqExecOrderInsert(&f, requestId);
  }
  case M_ReqExecOrderAction: {
    CThostFtdcInputExecOrderActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqExecOrderAction(&f, requestId);
  }
  case M_ReqForQuoteInsert: {
    CThostFtdcInputForQuoteField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqForQuoteInsert(&f, requestId);
  }
  case M_ReqQuoteInsert: {
    CThostFtdcInputQuoteField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQuoteInsert(&f, requestId);
  }
  case M_ReqQuoteAction: {
    CThostFtdcInputQuoteActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
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
    return api->ReqOptionSelfCloseInsert(&f, requestId);
  }
  case M_ReqOptionSelfCloseAction: {
    CThostFtdcInputOptionSelfCloseActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqOptionSelfCloseAction(&f, requestId);
  }
  case M_ReqCombActionInsert: {
    CThostFtdcInputCombActionField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
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
    return api->ReqQryInstrumentMarginRate(&f, requestId);
  }
  case M_ReqQryInstrumentCommissionRate: {
    CThostFtdcQryInstrumentCommissionRateField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
    return api->ReqQryInstrumentCommissionRate(&f, requestId);
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
    return api->ReqQryExchangeMarginRate(&f, requestId);
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
    return api->ReqFromBankToFutureByFuture(&f, requestId);
  }
  case M_ReqFromFutureToBankByFuture: {
    CThostFtdcReqTransferField f;
    std::memset(&f, 0, sizeof(f));
    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));
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
  default:
    return -1;
  }
}

} // namespace ctp
