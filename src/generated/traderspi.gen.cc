/* AUTO-GENERATED. Do not edit. */
#include "traderspi.gen.h"
#include "structids.gen.h"

namespace ctp {

static int eid(CThostFtdcRspInfoField *e) { return e ? e->ErrorID : 0; }
static const char *emsg(CThostFtdcRspInfoField *e) { return (e && e->ErrorID) ? e->ErrorMsg : ""; }

void TraderSpi::OnFrontConnected() {
  ch_->push(ET_FrontConnected, 0, -1, 0, "", -1, nullptr, 0);
}

void TraderSpi::OnFrontDisconnected(int arg) {
  ch_->push(ET_FrontDisconnected, 0, -1, 0, "", -2, &arg, sizeof(arg));
}

void TraderSpi::OnHeartBeatWarning(int arg) {
  ch_->push(ET_HeartBeatWarning, 0, -1, 0, "", -2, &arg, sizeof(arg));
}

void TraderSpi::OnRspAuthenticate(CThostFtdcRspAuthenticateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspAuthenticate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RspAuthenticate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnPrivateSeqNo(int arg) {
  ch_->push(ET_RtnPrivateSeqNo, 0, -1, 0, "", -2, &arg, sizeof(arg));
}

void TraderSpi::OnRspUserLogin(CThostFtdcRspUserLoginField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  if (p && risk_)
    risk_->setSession(p->FrontID, p->SessionID);
  ch_->push(ET_RspUserLogin, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RspUserLogin : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspUserLogout(CThostFtdcUserLogoutField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspUserLogout, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_UserLogout : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspUserPasswordUpdate(CThostFtdcUserPasswordUpdateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspUserPasswordUpdate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_UserPasswordUpdate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspTradingAccountPasswordUpdate(CThostFtdcTradingAccountPasswordUpdateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspTradingAccountPasswordUpdate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_TradingAccountPasswordUpdate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspUserAuthMethod(CThostFtdcRspUserAuthMethodField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspUserAuthMethod, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RspUserAuthMethod : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspGenUserCaptcha(CThostFtdcRspGenUserCaptchaField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspGenUserCaptcha, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RspGenUserCaptcha : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspGenUserText(CThostFtdcRspGenUserTextField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspGenUserText, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RspGenUserText : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspOrderInsert(CThostFtdcInputOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  if (p && risk_)
    risk_->releaseReservation(p->OrderRef);
  ch_->push(ET_RspOrderInsert, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputOrder : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspParkedOrderInsert(CThostFtdcParkedOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspParkedOrderInsert, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ParkedOrder : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspParkedOrderAction(CThostFtdcParkedOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspParkedOrderAction, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ParkedOrderAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspOrderAction(CThostFtdcInputOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspOrderAction, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputOrderAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryMaxOrderVolume(CThostFtdcQryMaxOrderVolumeField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryMaxOrderVolume, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_QryMaxOrderVolume : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspSettlementInfoConfirm, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SettlementInfoConfirm : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspRemoveParkedOrder(CThostFtdcRemoveParkedOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspRemoveParkedOrder, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RemoveParkedOrder : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspRemoveParkedOrderAction(CThostFtdcRemoveParkedOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspRemoveParkedOrderAction, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RemoveParkedOrderAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspExecOrderInsert(CThostFtdcInputExecOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspExecOrderInsert, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputExecOrder : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspExecOrderAction(CThostFtdcInputExecOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspExecOrderAction, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputExecOrderAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspForQuoteInsert(CThostFtdcInputForQuoteField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspForQuoteInsert, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputForQuote : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQuoteInsert(CThostFtdcInputQuoteField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQuoteInsert, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputQuote : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQuoteAction(CThostFtdcInputQuoteActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQuoteAction, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputQuoteAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspBatchOrderAction(CThostFtdcInputBatchOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspBatchOrderAction, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputBatchOrderAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspOptionSelfCloseInsert(CThostFtdcInputOptionSelfCloseField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspOptionSelfCloseInsert, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputOptionSelfClose : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspOptionSelfCloseAction(CThostFtdcInputOptionSelfCloseActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspOptionSelfCloseAction, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputOptionSelfCloseAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspCombActionInsert(CThostFtdcInputCombActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspCombActionInsert, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputCombAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryOrder(CThostFtdcOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryOrder, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_Order : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryTrade(CThostFtdcTradeField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryTrade, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_Trade : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorPosition, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorPosition : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryTradingAccount(CThostFtdcTradingAccountField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryTradingAccount, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_TradingAccount : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestor(CThostFtdcInvestorField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestor, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_Investor : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryTradingCode(CThostFtdcTradingCodeField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryTradingCode, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_TradingCode : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInstrumentMarginRate(CThostFtdcInstrumentMarginRateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  if (p && risk_) {
    const double mr = p->LongMarginRatioByMoney > p->ShortMarginRatioByMoney ? p->LongMarginRatioByMoney : p->ShortMarginRatioByMoney;
    risk_->setMarginRate(p->InstrumentID, mr);
  }
  ch_->push(ET_RspQryInstrumentMarginRate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InstrumentMarginRate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInstrumentCommissionRate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InstrumentCommissionRate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryUserSession(CThostFtdcUserSessionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryUserSession, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_UserSession : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryExchange(CThostFtdcExchangeField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryExchange, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_Exchange : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryProduct(CThostFtdcProductField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryProduct, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_Product : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInstrument(CThostFtdcInstrumentField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  if (p && risk_)
    risk_->setMultiplier(p->InstrumentID, p->VolumeMultiple);
  ch_->push(ET_RspQryInstrument, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_Instrument : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryDepthMarketData(CThostFtdcDepthMarketDataField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryDepthMarketData, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_DepthMarketData : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryTraderOffer(CThostFtdcTraderOfferField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryTraderOffer, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_TraderOffer : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySettlementInfo, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SettlementInfo : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryTransferBank(CThostFtdcTransferBankField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryTransferBank, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_TransferBank : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorPositionDetail(CThostFtdcInvestorPositionDetailField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorPositionDetail, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorPositionDetail : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryNotice(CThostFtdcNoticeField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryNotice, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_Notice : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySettlementInfoConfirm, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SettlementInfoConfirm : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorPositionCombineDetail(CThostFtdcInvestorPositionCombineDetailField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorPositionCombineDetail, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorPositionCombineDetail : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryCFMMCTradingAccountKey(CThostFtdcCFMMCTradingAccountKeyField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryCFMMCTradingAccountKey, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_CFMMCTradingAccountKey : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryEWarrantOffset(CThostFtdcEWarrantOffsetField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryEWarrantOffset, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_EWarrantOffset : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorProductGroupMargin(CThostFtdcInvestorProductGroupMarginField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorProductGroupMargin, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorProductGroupMargin : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryExchangeMarginRate(CThostFtdcExchangeMarginRateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryExchangeMarginRate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ExchangeMarginRate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryExchangeMarginRateAdjust(CThostFtdcExchangeMarginRateAdjustField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryExchangeMarginRateAdjust, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ExchangeMarginRateAdjust : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryExchangeRate(CThostFtdcExchangeRateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryExchangeRate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ExchangeRate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySecAgentACIDMap(CThostFtdcSecAgentACIDMapField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySecAgentACIDMap, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SecAgentACIDMap : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryProductExchRate(CThostFtdcProductExchRateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryProductExchRate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ProductExchRate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryProductGroup(CThostFtdcProductGroupField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryProductGroup, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ProductGroup : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryMMInstrumentCommissionRate(CThostFtdcMMInstrumentCommissionRateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryMMInstrumentCommissionRate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_MMInstrumentCommissionRate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryMMOptionInstrCommRate(CThostFtdcMMOptionInstrCommRateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryMMOptionInstrCommRate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_MMOptionInstrCommRate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInstrumentOrderCommRate(CThostFtdcInstrumentOrderCommRateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInstrumentOrderCommRate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InstrumentOrderCommRate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySecAgentTradingAccount(CThostFtdcTradingAccountField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySecAgentTradingAccount, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_TradingAccount : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySecAgentCheckMode(CThostFtdcSecAgentCheckModeField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySecAgentCheckMode, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SecAgentCheckMode : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySecAgentTradeInfo(CThostFtdcSecAgentTradeInfoField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySecAgentTradeInfo, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SecAgentTradeInfo : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryOptionInstrTradeCost(CThostFtdcOptionInstrTradeCostField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryOptionInstrTradeCost, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_OptionInstrTradeCost : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryOptionInstrCommRate(CThostFtdcOptionInstrCommRateField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryOptionInstrCommRate, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_OptionInstrCommRate : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryExecOrder(CThostFtdcExecOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryExecOrder, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ExecOrder : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryForQuote(CThostFtdcForQuoteField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryForQuote, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ForQuote : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryQuote(CThostFtdcQuoteField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryQuote, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_Quote : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryOptionSelfClose(CThostFtdcOptionSelfCloseField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryOptionSelfClose, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_OptionSelfClose : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestUnit(CThostFtdcInvestUnitField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestUnit, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestUnit : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryCombInstrumentGuard(CThostFtdcCombInstrumentGuardField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryCombInstrumentGuard, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_CombInstrumentGuard : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryCombAction(CThostFtdcCombActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryCombAction, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_CombAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryTransferSerial(CThostFtdcTransferSerialField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryTransferSerial, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_TransferSerial : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryAccountregister(CThostFtdcAccountregisterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryAccountregister, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_Accountregister : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspError(CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspError, id, last ? 1 : 0, eid(e), emsg(e), -1, nullptr, 0);
}

void TraderSpi::OnRtnOrder(CThostFtdcOrderField *p) {
  if (p && risk_)
    risk_->onOrderUpdate(p->FrontID, p->SessionID, p->OrderRef, p->InstrumentID, p->CombOffsetFlag[0] == '0', p->Direction == '0', p->OrderStatus, p->LimitPrice, p->VolumeTotalOriginal, p->VolumeTraded);
  ch_->push(ET_RtnOrder, 0, -1, 0, "", p ? SID_Order : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnTrade(CThostFtdcTradeField *p) {
  if (p && risk_)
    risk_->onTrade(p->InstrumentID, p->Direction == '0', p->OffsetFlag == '0', p->Price, p->Volume);
  ch_->push(ET_RtnTrade, 0, -1, 0, "", p ? SID_Trade : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnOrderInsert(CThostFtdcInputOrderField *p, CThostFtdcRspInfoField *e) {
  if (p && risk_)
    risk_->releaseReservation(p->OrderRef);
  ch_->push(ET_ErrRtnOrderInsert, 0, -1, eid(e), emsg(e), p ? SID_InputOrder : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnOrderAction(CThostFtdcOrderActionField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnOrderAction, 0, -1, eid(e), emsg(e), p ? SID_OrderAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *p) {
  ch_->push(ET_RtnInstrumentStatus, 0, -1, 0, "", p ? SID_InstrumentStatus : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnBulletin(CThostFtdcBulletinField *p) {
  ch_->push(ET_RtnBulletin, 0, -1, 0, "", p ? SID_Bulletin : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnTradingNotice(CThostFtdcTradingNoticeInfoField *p) {
  ch_->push(ET_RtnTradingNotice, 0, -1, 0, "", p ? SID_TradingNoticeInfo : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnErrorConditionalOrder(CThostFtdcErrorConditionalOrderField *p) {
  ch_->push(ET_RtnErrorConditionalOrder, 0, -1, 0, "", p ? SID_ErrorConditionalOrder : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnExecOrder(CThostFtdcExecOrderField *p) {
  ch_->push(ET_RtnExecOrder, 0, -1, 0, "", p ? SID_ExecOrder : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnExecOrderInsert(CThostFtdcInputExecOrderField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnExecOrderInsert, 0, -1, eid(e), emsg(e), p ? SID_InputExecOrder : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnExecOrderAction(CThostFtdcExecOrderActionField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnExecOrderAction, 0, -1, eid(e), emsg(e), p ? SID_ExecOrderAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnForQuoteInsert(CThostFtdcInputForQuoteField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnForQuoteInsert, 0, -1, eid(e), emsg(e), p ? SID_InputForQuote : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnQuote(CThostFtdcQuoteField *p) {
  ch_->push(ET_RtnQuote, 0, -1, 0, "", p ? SID_Quote : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnQuoteInsert(CThostFtdcInputQuoteField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnQuoteInsert, 0, -1, eid(e), emsg(e), p ? SID_InputQuote : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnQuoteAction(CThostFtdcQuoteActionField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnQuoteAction, 0, -1, eid(e), emsg(e), p ? SID_QuoteAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnForQuoteRsp(CThostFtdcForQuoteRspField *p) {
  ch_->push(ET_RtnForQuoteRsp, 0, -1, 0, "", p ? SID_ForQuoteRsp : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnCFMMCTradingAccountToken(CThostFtdcCFMMCTradingAccountTokenField *p) {
  ch_->push(ET_RtnCFMMCTradingAccountToken, 0, -1, 0, "", p ? SID_CFMMCTradingAccountToken : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnBatchOrderAction(CThostFtdcBatchOrderActionField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnBatchOrderAction, 0, -1, eid(e), emsg(e), p ? SID_BatchOrderAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnOptionSelfClose(CThostFtdcOptionSelfCloseField *p) {
  ch_->push(ET_RtnOptionSelfClose, 0, -1, 0, "", p ? SID_OptionSelfClose : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnOptionSelfCloseInsert(CThostFtdcInputOptionSelfCloseField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnOptionSelfCloseInsert, 0, -1, eid(e), emsg(e), p ? SID_InputOptionSelfClose : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnOptionSelfCloseAction(CThostFtdcOptionSelfCloseActionField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnOptionSelfCloseAction, 0, -1, eid(e), emsg(e), p ? SID_OptionSelfCloseAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnCombAction(CThostFtdcCombActionField *p) {
  ch_->push(ET_RtnCombAction, 0, -1, 0, "", p ? SID_CombAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnCombActionInsert(CThostFtdcInputCombActionField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnCombActionInsert, 0, -1, eid(e), emsg(e), p ? SID_InputCombAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryContractBank(CThostFtdcContractBankField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryContractBank, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ContractBank : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryParkedOrder(CThostFtdcParkedOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryParkedOrder, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ParkedOrder : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryParkedOrderAction(CThostFtdcParkedOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryParkedOrderAction, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ParkedOrderAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryTradingNotice(CThostFtdcTradingNoticeField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryTradingNotice, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_TradingNotice : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryBrokerTradingParams(CThostFtdcBrokerTradingParamsField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryBrokerTradingParams, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_BrokerTradingParams : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryBrokerTradingAlgos(CThostFtdcBrokerTradingAlgosField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryBrokerTradingAlgos, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_BrokerTradingAlgos : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQueryCFMMCTradingAccountToken(CThostFtdcQueryCFMMCTradingAccountTokenField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQueryCFMMCTradingAccountToken, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_QueryCFMMCTradingAccountToken : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnFromBankToFutureByBank(CThostFtdcRspTransferField *p) {
  ch_->push(ET_RtnFromBankToFutureByBank, 0, -1, 0, "", p ? SID_RspTransfer : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnFromFutureToBankByBank(CThostFtdcRspTransferField *p) {
  ch_->push(ET_RtnFromFutureToBankByBank, 0, -1, 0, "", p ? SID_RspTransfer : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnRepealFromBankToFutureByBank(CThostFtdcRspRepealField *p) {
  ch_->push(ET_RtnRepealFromBankToFutureByBank, 0, -1, 0, "", p ? SID_RspRepeal : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnRepealFromFutureToBankByBank(CThostFtdcRspRepealField *p) {
  ch_->push(ET_RtnRepealFromFutureToBankByBank, 0, -1, 0, "", p ? SID_RspRepeal : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnFromBankToFutureByFuture(CThostFtdcRspTransferField *p) {
  ch_->push(ET_RtnFromBankToFutureByFuture, 0, -1, 0, "", p ? SID_RspTransfer : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnFromFutureToBankByFuture(CThostFtdcRspTransferField *p) {
  ch_->push(ET_RtnFromFutureToBankByFuture, 0, -1, 0, "", p ? SID_RspTransfer : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnRepealFromBankToFutureByFutureManual(CThostFtdcRspRepealField *p) {
  ch_->push(ET_RtnRepealFromBankToFutureByFutureManual, 0, -1, 0, "", p ? SID_RspRepeal : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnRepealFromFutureToBankByFutureManual(CThostFtdcRspRepealField *p) {
  ch_->push(ET_RtnRepealFromFutureToBankByFutureManual, 0, -1, 0, "", p ? SID_RspRepeal : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnQueryBankBalanceByFuture(CThostFtdcNotifyQueryAccountField *p) {
  ch_->push(ET_RtnQueryBankBalanceByFuture, 0, -1, 0, "", p ? SID_NotifyQueryAccount : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnBankToFutureByFuture(CThostFtdcReqTransferField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnBankToFutureByFuture, 0, -1, eid(e), emsg(e), p ? SID_ReqTransfer : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnFutureToBankByFuture(CThostFtdcReqTransferField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnFutureToBankByFuture, 0, -1, eid(e), emsg(e), p ? SID_ReqTransfer : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnRepealBankToFutureByFutureManual(CThostFtdcReqRepealField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnRepealBankToFutureByFutureManual, 0, -1, eid(e), emsg(e), p ? SID_ReqRepeal : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnRepealFutureToBankByFutureManual(CThostFtdcReqRepealField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnRepealFutureToBankByFutureManual, 0, -1, eid(e), emsg(e), p ? SID_ReqRepeal : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnQueryBankBalanceByFuture(CThostFtdcReqQueryAccountField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnQueryBankBalanceByFuture, 0, -1, eid(e), emsg(e), p ? SID_ReqQueryAccount : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnRepealFromBankToFutureByFuture(CThostFtdcRspRepealField *p) {
  ch_->push(ET_RtnRepealFromBankToFutureByFuture, 0, -1, 0, "", p ? SID_RspRepeal : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnRepealFromFutureToBankByFuture(CThostFtdcRspRepealField *p) {
  ch_->push(ET_RtnRepealFromFutureToBankByFuture, 0, -1, 0, "", p ? SID_RspRepeal : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspFromBankToFutureByFuture(CThostFtdcReqTransferField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspFromBankToFutureByFuture, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ReqTransfer : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspFromFutureToBankByFuture(CThostFtdcReqTransferField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspFromFutureToBankByFuture, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ReqTransfer : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQueryBankAccountMoneyByFuture(CThostFtdcReqQueryAccountField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQueryBankAccountMoneyByFuture, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_ReqQueryAccount : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnOpenAccountByBank(CThostFtdcOpenAccountField *p) {
  ch_->push(ET_RtnOpenAccountByBank, 0, -1, 0, "", p ? SID_OpenAccount : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnCancelAccountByBank(CThostFtdcCancelAccountField *p) {
  ch_->push(ET_RtnCancelAccountByBank, 0, -1, 0, "", p ? SID_CancelAccount : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnChangeAccountByBank(CThostFtdcChangeAccountField *p) {
  ch_->push(ET_RtnChangeAccountByBank, 0, -1, 0, "", p ? SID_ChangeAccount : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryClassifiedInstrument(CThostFtdcInstrumentField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryClassifiedInstrument, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_Instrument : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryCombPromotionParam(CThostFtdcCombPromotionParamField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryCombPromotionParam, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_CombPromotionParam : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryRiskSettleInvstPosition(CThostFtdcRiskSettleInvstPositionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryRiskSettleInvstPosition, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RiskSettleInvstPosition : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryRiskSettleProductStatus(CThostFtdcRiskSettleProductStatusField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryRiskSettleProductStatus, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RiskSettleProductStatus : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySPBMFutureParameter(CThostFtdcSPBMFutureParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySPBMFutureParameter, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SPBMFutureParameter : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySPBMOptionParameter(CThostFtdcSPBMOptionParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySPBMOptionParameter, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SPBMOptionParameter : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySPBMIntraParameter(CThostFtdcSPBMIntraParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySPBMIntraParameter, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SPBMIntraParameter : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySPBMInterParameter(CThostFtdcSPBMInterParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySPBMInterParameter, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SPBMInterParameter : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySPBMPortfDefinition(CThostFtdcSPBMPortfDefinitionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySPBMPortfDefinition, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SPBMPortfDefinition : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySPBMInvestorPortfDef(CThostFtdcSPBMInvestorPortfDefField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySPBMInvestorPortfDef, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SPBMInvestorPortfDef : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorPortfMarginRatio(CThostFtdcInvestorPortfMarginRatioField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorPortfMarginRatio, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorPortfMarginRatio : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorProdSPBMDetail(CThostFtdcInvestorProdSPBMDetailField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorProdSPBMDetail, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorProdSPBMDetail : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorCommoditySPMMMargin(CThostFtdcInvestorCommoditySPMMMarginField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorCommoditySPMMMargin, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorCommoditySPMMMargin : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorCommodityGroupSPMMMargin(CThostFtdcInvestorCommodityGroupSPMMMarginField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorCommodityGroupSPMMMargin, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorCommodityGroupSPMMMargin : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySPMMInstParam(CThostFtdcSPMMInstParamField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySPMMInstParam, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SPMMInstParam : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySPMMProductParam(CThostFtdcSPMMProductParamField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySPMMProductParam, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SPMMProductParam : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySPBMAddOnInterParameter(CThostFtdcSPBMAddOnInterParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySPBMAddOnInterParameter, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SPBMAddOnInterParameter : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryRCAMSCombProductInfo(CThostFtdcRCAMSCombProductInfoField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryRCAMSCombProductInfo, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RCAMSCombProductInfo : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryRCAMSInstrParameter(CThostFtdcRCAMSInstrParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryRCAMSInstrParameter, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RCAMSInstrParameter : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryRCAMSIntraParameter(CThostFtdcRCAMSIntraParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryRCAMSIntraParameter, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RCAMSIntraParameter : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryRCAMSInterParameter(CThostFtdcRCAMSInterParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryRCAMSInterParameter, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RCAMSInterParameter : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryRCAMSShortOptAdjustParam(CThostFtdcRCAMSShortOptAdjustParamField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryRCAMSShortOptAdjustParam, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RCAMSShortOptAdjustParam : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryRCAMSInvestorCombPosition(CThostFtdcRCAMSInvestorCombPositionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryRCAMSInvestorCombPosition, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RCAMSInvestorCombPosition : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorProdRCAMSMargin(CThostFtdcInvestorProdRCAMSMarginField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorProdRCAMSMargin, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorProdRCAMSMargin : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryRULEInstrParameter(CThostFtdcRULEInstrParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryRULEInstrParameter, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RULEInstrParameter : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryRULEIntraParameter(CThostFtdcRULEIntraParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryRULEIntraParameter, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RULEIntraParameter : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryRULEInterParameter(CThostFtdcRULEInterParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryRULEInterParameter, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RULEInterParameter : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorProdRULEMargin(CThostFtdcInvestorProdRULEMarginField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorProdRULEMargin, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorProdRULEMargin : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorPortfSetting(CThostFtdcInvestorPortfSettingField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorPortfSetting, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorPortfSetting : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryInvestorInfoCommRec(CThostFtdcInvestorInfoCommRecField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryInvestorInfoCommRec, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InvestorInfoCommRec : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryCombLeg(CThostFtdcCombLegField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryCombLeg, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_CombLeg : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspOffsetSetting(CThostFtdcInputOffsetSettingField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspOffsetSetting, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputOffsetSetting : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspCancelOffsetSetting(CThostFtdcInputOffsetSettingField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspCancelOffsetSetting, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputOffsetSetting : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnOffsetSetting(CThostFtdcOffsetSettingField *p) {
  ch_->push(ET_RtnOffsetSetting, 0, -1, 0, "", p ? SID_OffsetSetting : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnOffsetSetting(CThostFtdcInputOffsetSettingField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnOffsetSetting, 0, -1, eid(e), emsg(e), p ? SID_InputOffsetSetting : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnCancelOffsetSetting(CThostFtdcCancelOffsetSettingField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnCancelOffsetSetting, 0, -1, eid(e), emsg(e), p ? SID_CancelOffsetSetting : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryOffsetSetting(CThostFtdcOffsetSettingField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryOffsetSetting, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_OffsetSetting : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspGenSMSCode(CThostFtdcRspGenSMSCodeField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspGenSMSCode, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_RspGenSMSCode : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspSpdApply(CThostFtdcInputSpdApplyField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspSpdApply, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputSpdApply : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspSpdApplyAction(CThostFtdcInputSpdApplyActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspSpdApplyAction, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputSpdApplyAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQrySpdApply(CThostFtdcSpdApplyField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQrySpdApply, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_SpdApply : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnSpdApply(CThostFtdcSpdApplyField *p) {
  ch_->push(ET_RtnSpdApply, 0, -1, 0, "", p ? SID_SpdApply : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnSpdApply(CThostFtdcInputSpdApplyField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnSpdApply, 0, -1, eid(e), emsg(e), p ? SID_InputSpdApply : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnSpdApplyAction(CThostFtdcSpdApplyActionField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnSpdApplyAction, 0, -1, eid(e), emsg(e), p ? SID_SpdApplyAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspHedgeCfm(CThostFtdcInputHedgeCfmField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspHedgeCfm, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputHedgeCfm : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspHedgeCfmAction(CThostFtdcInputHedgeCfmActionField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspHedgeCfmAction, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_InputHedgeCfmAction : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRspQryHedgeCfm(CThostFtdcHedgeCfmField *p, CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(ET_RspQryHedgeCfm, id, last ? 1 : 0, eid(e), emsg(e), p ? SID_HedgeCfm : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnRtnHedgeCfm(CThostFtdcHedgeCfmField *p) {
  ch_->push(ET_RtnHedgeCfm, 0, -1, 0, "", p ? SID_HedgeCfm : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnHedgeCfm(CThostFtdcInputHedgeCfmField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnHedgeCfm, 0, -1, eid(e), emsg(e), p ? SID_InputHedgeCfm : -1, p, p ? (int)sizeof(*p) : 0);
}

void TraderSpi::OnErrRtnHedgeCfmAction(CThostFtdcHedgeCfmActionField *p, CThostFtdcRspInfoField *e) {
  ch_->push(ET_ErrRtnHedgeCfmAction, 0, -1, eid(e), emsg(e), p ? SID_HedgeCfmAction : -1, p, p ? (int)sizeof(*p) : 0);
}

} // namespace ctp
