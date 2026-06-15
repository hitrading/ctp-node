/* AUTO-GENERATED. Do not edit. */
#ifndef CTP_TRADERSPI_GEN_H
#define CTP_TRADERSPI_GEN_H
#include "ThostFtdcTraderApi.h"
#include "../native/channel.h"
namespace ctp {
enum {
  ET_BASE = 8192,
  ET_FrontConnected = ET_BASE + 1,
  ET_FrontDisconnected = ET_BASE + 2,
  ET_HeartBeatWarning = ET_BASE + 3,
  ET_RspAuthenticate = ET_BASE + 4,
  ET_RspUserLogin = ET_BASE + 5,
  ET_RspUserLogout = ET_BASE + 6,
  ET_RspUserPasswordUpdate = ET_BASE + 7,
  ET_RspTradingAccountPasswordUpdate = ET_BASE + 8,
  ET_RspUserAuthMethod = ET_BASE + 9,
  ET_RspGenUserCaptcha = ET_BASE + 10,
  ET_RspGenUserText = ET_BASE + 11,
  ET_RspOrderInsert = ET_BASE + 12,
  ET_RspParkedOrderInsert = ET_BASE + 13,
  ET_RspParkedOrderAction = ET_BASE + 14,
  ET_RspOrderAction = ET_BASE + 15,
  ET_RspQryMaxOrderVolume = ET_BASE + 16,
  ET_RspSettlementInfoConfirm = ET_BASE + 17,
  ET_RspRemoveParkedOrder = ET_BASE + 18,
  ET_RspRemoveParkedOrderAction = ET_BASE + 19,
  ET_RspExecOrderInsert = ET_BASE + 20,
  ET_RspExecOrderAction = ET_BASE + 21,
  ET_RspForQuoteInsert = ET_BASE + 22,
  ET_RspQuoteInsert = ET_BASE + 23,
  ET_RspQuoteAction = ET_BASE + 24,
  ET_RspBatchOrderAction = ET_BASE + 25,
  ET_RspOptionSelfCloseInsert = ET_BASE + 26,
  ET_RspOptionSelfCloseAction = ET_BASE + 27,
  ET_RspCombActionInsert = ET_BASE + 28,
  ET_RspQryOrder = ET_BASE + 29,
  ET_RspQryTrade = ET_BASE + 30,
  ET_RspQryInvestorPosition = ET_BASE + 31,
  ET_RspQryTradingAccount = ET_BASE + 32,
  ET_RspQryInvestor = ET_BASE + 33,
  ET_RspQryTradingCode = ET_BASE + 34,
  ET_RspQryInstrumentMarginRate = ET_BASE + 35,
  ET_RspQryInstrumentCommissionRate = ET_BASE + 36,
  ET_RspQryExchange = ET_BASE + 37,
  ET_RspQryProduct = ET_BASE + 38,
  ET_RspQryInstrument = ET_BASE + 39,
  ET_RspQryDepthMarketData = ET_BASE + 40,
  ET_RspQryTraderOffer = ET_BASE + 41,
  ET_RspQrySettlementInfo = ET_BASE + 42,
  ET_RspQryTransferBank = ET_BASE + 43,
  ET_RspQryInvestorPositionDetail = ET_BASE + 44,
  ET_RspQryNotice = ET_BASE + 45,
  ET_RspQrySettlementInfoConfirm = ET_BASE + 46,
  ET_RspQryInvestorPositionCombineDetail = ET_BASE + 47,
  ET_RspQryCFMMCTradingAccountKey = ET_BASE + 48,
  ET_RspQryEWarrantOffset = ET_BASE + 49,
  ET_RspQryInvestorProductGroupMargin = ET_BASE + 50,
  ET_RspQryExchangeMarginRate = ET_BASE + 51,
  ET_RspQryExchangeMarginRateAdjust = ET_BASE + 52,
  ET_RspQryExchangeRate = ET_BASE + 53,
  ET_RspQrySecAgentACIDMap = ET_BASE + 54,
  ET_RspQryProductExchRate = ET_BASE + 55,
  ET_RspQryProductGroup = ET_BASE + 56,
  ET_RspQryMMInstrumentCommissionRate = ET_BASE + 57,
  ET_RspQryMMOptionInstrCommRate = ET_BASE + 58,
  ET_RspQryInstrumentOrderCommRate = ET_BASE + 59,
  ET_RspQrySecAgentTradingAccount = ET_BASE + 60,
  ET_RspQrySecAgentCheckMode = ET_BASE + 61,
  ET_RspQrySecAgentTradeInfo = ET_BASE + 62,
  ET_RspQryOptionInstrTradeCost = ET_BASE + 63,
  ET_RspQryOptionInstrCommRate = ET_BASE + 64,
  ET_RspQryExecOrder = ET_BASE + 65,
  ET_RspQryForQuote = ET_BASE + 66,
  ET_RspQryQuote = ET_BASE + 67,
  ET_RspQryOptionSelfClose = ET_BASE + 68,
  ET_RspQryInvestUnit = ET_BASE + 69,
  ET_RspQryCombInstrumentGuard = ET_BASE + 70,
  ET_RspQryCombAction = ET_BASE + 71,
  ET_RspQryTransferSerial = ET_BASE + 72,
  ET_RspQryAccountregister = ET_BASE + 73,
  ET_RspError = ET_BASE + 74,
  ET_RtnOrder = ET_BASE + 75,
  ET_RtnTrade = ET_BASE + 76,
  ET_ErrRtnOrderInsert = ET_BASE + 77,
  ET_ErrRtnOrderAction = ET_BASE + 78,
  ET_RtnInstrumentStatus = ET_BASE + 79,
  ET_RtnBulletin = ET_BASE + 80,
  ET_RtnTradingNotice = ET_BASE + 81,
  ET_RtnErrorConditionalOrder = ET_BASE + 82,
  ET_RtnExecOrder = ET_BASE + 83,
  ET_ErrRtnExecOrderInsert = ET_BASE + 84,
  ET_ErrRtnExecOrderAction = ET_BASE + 85,
  ET_ErrRtnForQuoteInsert = ET_BASE + 86,
  ET_RtnQuote = ET_BASE + 87,
  ET_ErrRtnQuoteInsert = ET_BASE + 88,
  ET_ErrRtnQuoteAction = ET_BASE + 89,
  ET_RtnForQuoteRsp = ET_BASE + 90,
  ET_RtnCFMMCTradingAccountToken = ET_BASE + 91,
  ET_ErrRtnBatchOrderAction = ET_BASE + 92,
  ET_RtnOptionSelfClose = ET_BASE + 93,
  ET_ErrRtnOptionSelfCloseInsert = ET_BASE + 94,
  ET_ErrRtnOptionSelfCloseAction = ET_BASE + 95,
  ET_RtnCombAction = ET_BASE + 96,
  ET_ErrRtnCombActionInsert = ET_BASE + 97,
  ET_RspQryContractBank = ET_BASE + 98,
  ET_RspQryParkedOrder = ET_BASE + 99,
  ET_RspQryParkedOrderAction = ET_BASE + 100,
  ET_RspQryTradingNotice = ET_BASE + 101,
  ET_RspQryBrokerTradingParams = ET_BASE + 102,
  ET_RspQryBrokerTradingAlgos = ET_BASE + 103,
  ET_RspQueryCFMMCTradingAccountToken = ET_BASE + 104,
  ET_RtnFromBankToFutureByBank = ET_BASE + 105,
  ET_RtnFromFutureToBankByBank = ET_BASE + 106,
  ET_RtnRepealFromBankToFutureByBank = ET_BASE + 107,
  ET_RtnRepealFromFutureToBankByBank = ET_BASE + 108,
  ET_RtnFromBankToFutureByFuture = ET_BASE + 109,
  ET_RtnFromFutureToBankByFuture = ET_BASE + 110,
  ET_RtnRepealFromBankToFutureByFutureManual = ET_BASE + 111,
  ET_RtnRepealFromFutureToBankByFutureManual = ET_BASE + 112,
  ET_RtnQueryBankBalanceByFuture = ET_BASE + 113,
  ET_ErrRtnBankToFutureByFuture = ET_BASE + 114,
  ET_ErrRtnFutureToBankByFuture = ET_BASE + 115,
  ET_ErrRtnRepealBankToFutureByFutureManual = ET_BASE + 116,
  ET_ErrRtnRepealFutureToBankByFutureManual = ET_BASE + 117,
  ET_ErrRtnQueryBankBalanceByFuture = ET_BASE + 118,
  ET_RtnRepealFromBankToFutureByFuture = ET_BASE + 119,
  ET_RtnRepealFromFutureToBankByFuture = ET_BASE + 120,
  ET_RspFromBankToFutureByFuture = ET_BASE + 121,
  ET_RspFromFutureToBankByFuture = ET_BASE + 122,
  ET_RspQueryBankAccountMoneyByFuture = ET_BASE + 123,
  ET_RtnOpenAccountByBank = ET_BASE + 124,
  ET_RtnCancelAccountByBank = ET_BASE + 125,
  ET_RtnChangeAccountByBank = ET_BASE + 126,
  ET_RspQryClassifiedInstrument = ET_BASE + 127,
  ET_RspQryCombPromotionParam = ET_BASE + 128,
  ET_RspQryRiskSettleInvstPosition = ET_BASE + 129,
  ET_RspQryRiskSettleProductStatus = ET_BASE + 130,
  ET_RspQrySPBMFutureParameter = ET_BASE + 131,
  ET_RspQrySPBMOptionParameter = ET_BASE + 132,
  ET_RspQrySPBMIntraParameter = ET_BASE + 133,
  ET_RspQrySPBMInterParameter = ET_BASE + 134,
  ET_RspQrySPBMPortfDefinition = ET_BASE + 135,
  ET_RspQrySPBMInvestorPortfDef = ET_BASE + 136,
  ET_RspQryInvestorPortfMarginRatio = ET_BASE + 137,
  ET_RspQryInvestorProdSPBMDetail = ET_BASE + 138,
  ET_RspQryInvestorCommoditySPMMMargin = ET_BASE + 139,
  ET_RspQryInvestorCommodityGroupSPMMMargin = ET_BASE + 140,
  ET_RspQrySPMMInstParam = ET_BASE + 141,
  ET_RspQrySPMMProductParam = ET_BASE + 142,
  ET_RspQrySPBMAddOnInterParameter = ET_BASE + 143,
  ET_RspQryRCAMSCombProductInfo = ET_BASE + 144,
  ET_RspQryRCAMSInstrParameter = ET_BASE + 145,
  ET_RspQryRCAMSIntraParameter = ET_BASE + 146,
  ET_RspQryRCAMSInterParameter = ET_BASE + 147,
  ET_RspQryRCAMSShortOptAdjustParam = ET_BASE + 148,
  ET_RspQryRCAMSInvestorCombPosition = ET_BASE + 149,
  ET_RspQryInvestorProdRCAMSMargin = ET_BASE + 150,
  ET_RspQryRULEInstrParameter = ET_BASE + 151,
  ET_RspQryRULEIntraParameter = ET_BASE + 152,
  ET_RspQryRULEInterParameter = ET_BASE + 153,
  ET_RspQryInvestorProdRULEMargin = ET_BASE + 154,
};

class TraderSpi : public CThostFtdcTraderSpi {
public:
  TraderSpi(CThostFtdcTraderApi *api, EventChannel *channel) : api_(api), ch_(channel) {}
  ~TraderSpi() {}

  void OnFrontConnected() override;
  void OnFrontDisconnected(int arg) override;
  void OnHeartBeatWarning(int arg) override;
  void OnRspAuthenticate(CThostFtdcRspAuthenticateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspUserLogin(CThostFtdcRspUserLoginField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspUserLogout(CThostFtdcUserLogoutField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspUserPasswordUpdate(CThostFtdcUserPasswordUpdateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspTradingAccountPasswordUpdate(CThostFtdcTradingAccountPasswordUpdateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspUserAuthMethod(CThostFtdcRspUserAuthMethodField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspGenUserCaptcha(CThostFtdcRspGenUserCaptchaField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspGenUserText(CThostFtdcRspGenUserTextField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspOrderInsert(CThostFtdcInputOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspParkedOrderInsert(CThostFtdcParkedOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspParkedOrderAction(CThostFtdcParkedOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspOrderAction(CThostFtdcInputOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryMaxOrderVolume(CThostFtdcQryMaxOrderVolumeField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspRemoveParkedOrder(CThostFtdcRemoveParkedOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspRemoveParkedOrderAction(CThostFtdcRemoveParkedOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspExecOrderInsert(CThostFtdcInputExecOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspExecOrderAction(CThostFtdcInputExecOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspForQuoteInsert(CThostFtdcInputForQuoteField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQuoteInsert(CThostFtdcInputQuoteField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQuoteAction(CThostFtdcInputQuoteActionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspBatchOrderAction(CThostFtdcInputBatchOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspOptionSelfCloseInsert(CThostFtdcInputOptionSelfCloseField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspOptionSelfCloseAction(CThostFtdcInputOptionSelfCloseActionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspCombActionInsert(CThostFtdcInputCombActionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryOrder(CThostFtdcOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryTrade(CThostFtdcTradeField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryTradingAccount(CThostFtdcTradingAccountField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestor(CThostFtdcInvestorField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryTradingCode(CThostFtdcTradingCodeField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInstrumentMarginRate(CThostFtdcInstrumentMarginRateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryExchange(CThostFtdcExchangeField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryProduct(CThostFtdcProductField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInstrument(CThostFtdcInstrumentField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryDepthMarketData(CThostFtdcDepthMarketDataField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryTraderOffer(CThostFtdcTraderOfferField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySettlementInfo(CThostFtdcSettlementInfoField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryTransferBank(CThostFtdcTransferBankField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestorPositionDetail(CThostFtdcInvestorPositionDetailField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryNotice(CThostFtdcNoticeField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestorPositionCombineDetail(CThostFtdcInvestorPositionCombineDetailField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryCFMMCTradingAccountKey(CThostFtdcCFMMCTradingAccountKeyField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryEWarrantOffset(CThostFtdcEWarrantOffsetField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestorProductGroupMargin(CThostFtdcInvestorProductGroupMarginField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryExchangeMarginRate(CThostFtdcExchangeMarginRateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryExchangeMarginRateAdjust(CThostFtdcExchangeMarginRateAdjustField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryExchangeRate(CThostFtdcExchangeRateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySecAgentACIDMap(CThostFtdcSecAgentACIDMapField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryProductExchRate(CThostFtdcProductExchRateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryProductGroup(CThostFtdcProductGroupField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryMMInstrumentCommissionRate(CThostFtdcMMInstrumentCommissionRateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryMMOptionInstrCommRate(CThostFtdcMMOptionInstrCommRateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInstrumentOrderCommRate(CThostFtdcInstrumentOrderCommRateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySecAgentTradingAccount(CThostFtdcTradingAccountField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySecAgentCheckMode(CThostFtdcSecAgentCheckModeField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySecAgentTradeInfo(CThostFtdcSecAgentTradeInfoField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryOptionInstrTradeCost(CThostFtdcOptionInstrTradeCostField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryOptionInstrCommRate(CThostFtdcOptionInstrCommRateField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryExecOrder(CThostFtdcExecOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryForQuote(CThostFtdcForQuoteField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryQuote(CThostFtdcQuoteField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryOptionSelfClose(CThostFtdcOptionSelfCloseField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestUnit(CThostFtdcInvestUnitField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryCombInstrumentGuard(CThostFtdcCombInstrumentGuardField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryCombAction(CThostFtdcCombActionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryTransferSerial(CThostFtdcTransferSerialField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryAccountregister(CThostFtdcAccountregisterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspError(CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRtnOrder(CThostFtdcOrderField *p) override;
  void OnRtnTrade(CThostFtdcTradeField *p) override;
  void OnErrRtnOrderInsert(CThostFtdcInputOrderField *p, CThostFtdcRspInfoField *e) override;
  void OnErrRtnOrderAction(CThostFtdcOrderActionField *p, CThostFtdcRspInfoField *e) override;
  void OnRtnInstrumentStatus(CThostFtdcInstrumentStatusField *p) override;
  void OnRtnBulletin(CThostFtdcBulletinField *p) override;
  void OnRtnTradingNotice(CThostFtdcTradingNoticeInfoField *p) override;
  void OnRtnErrorConditionalOrder(CThostFtdcErrorConditionalOrderField *p) override;
  void OnRtnExecOrder(CThostFtdcExecOrderField *p) override;
  void OnErrRtnExecOrderInsert(CThostFtdcInputExecOrderField *p, CThostFtdcRspInfoField *e) override;
  void OnErrRtnExecOrderAction(CThostFtdcExecOrderActionField *p, CThostFtdcRspInfoField *e) override;
  void OnErrRtnForQuoteInsert(CThostFtdcInputForQuoteField *p, CThostFtdcRspInfoField *e) override;
  void OnRtnQuote(CThostFtdcQuoteField *p) override;
  void OnErrRtnQuoteInsert(CThostFtdcInputQuoteField *p, CThostFtdcRspInfoField *e) override;
  void OnErrRtnQuoteAction(CThostFtdcQuoteActionField *p, CThostFtdcRspInfoField *e) override;
  void OnRtnForQuoteRsp(CThostFtdcForQuoteRspField *p) override;
  void OnRtnCFMMCTradingAccountToken(CThostFtdcCFMMCTradingAccountTokenField *p) override;
  void OnErrRtnBatchOrderAction(CThostFtdcBatchOrderActionField *p, CThostFtdcRspInfoField *e) override;
  void OnRtnOptionSelfClose(CThostFtdcOptionSelfCloseField *p) override;
  void OnErrRtnOptionSelfCloseInsert(CThostFtdcInputOptionSelfCloseField *p, CThostFtdcRspInfoField *e) override;
  void OnErrRtnOptionSelfCloseAction(CThostFtdcOptionSelfCloseActionField *p, CThostFtdcRspInfoField *e) override;
  void OnRtnCombAction(CThostFtdcCombActionField *p) override;
  void OnErrRtnCombActionInsert(CThostFtdcInputCombActionField *p, CThostFtdcRspInfoField *e) override;
  void OnRspQryContractBank(CThostFtdcContractBankField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryParkedOrder(CThostFtdcParkedOrderField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryParkedOrderAction(CThostFtdcParkedOrderActionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryTradingNotice(CThostFtdcTradingNoticeField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryBrokerTradingParams(CThostFtdcBrokerTradingParamsField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryBrokerTradingAlgos(CThostFtdcBrokerTradingAlgosField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQueryCFMMCTradingAccountToken(CThostFtdcQueryCFMMCTradingAccountTokenField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRtnFromBankToFutureByBank(CThostFtdcRspTransferField *p) override;
  void OnRtnFromFutureToBankByBank(CThostFtdcRspTransferField *p) override;
  void OnRtnRepealFromBankToFutureByBank(CThostFtdcRspRepealField *p) override;
  void OnRtnRepealFromFutureToBankByBank(CThostFtdcRspRepealField *p) override;
  void OnRtnFromBankToFutureByFuture(CThostFtdcRspTransferField *p) override;
  void OnRtnFromFutureToBankByFuture(CThostFtdcRspTransferField *p) override;
  void OnRtnRepealFromBankToFutureByFutureManual(CThostFtdcRspRepealField *p) override;
  void OnRtnRepealFromFutureToBankByFutureManual(CThostFtdcRspRepealField *p) override;
  void OnRtnQueryBankBalanceByFuture(CThostFtdcNotifyQueryAccountField *p) override;
  void OnErrRtnBankToFutureByFuture(CThostFtdcReqTransferField *p, CThostFtdcRspInfoField *e) override;
  void OnErrRtnFutureToBankByFuture(CThostFtdcReqTransferField *p, CThostFtdcRspInfoField *e) override;
  void OnErrRtnRepealBankToFutureByFutureManual(CThostFtdcReqRepealField *p, CThostFtdcRspInfoField *e) override;
  void OnErrRtnRepealFutureToBankByFutureManual(CThostFtdcReqRepealField *p, CThostFtdcRspInfoField *e) override;
  void OnErrRtnQueryBankBalanceByFuture(CThostFtdcReqQueryAccountField *p, CThostFtdcRspInfoField *e) override;
  void OnRtnRepealFromBankToFutureByFuture(CThostFtdcRspRepealField *p) override;
  void OnRtnRepealFromFutureToBankByFuture(CThostFtdcRspRepealField *p) override;
  void OnRspFromBankToFutureByFuture(CThostFtdcReqTransferField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspFromFutureToBankByFuture(CThostFtdcReqTransferField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQueryBankAccountMoneyByFuture(CThostFtdcReqQueryAccountField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRtnOpenAccountByBank(CThostFtdcOpenAccountField *p) override;
  void OnRtnCancelAccountByBank(CThostFtdcCancelAccountField *p) override;
  void OnRtnChangeAccountByBank(CThostFtdcChangeAccountField *p) override;
  void OnRspQryClassifiedInstrument(CThostFtdcInstrumentField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryCombPromotionParam(CThostFtdcCombPromotionParamField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryRiskSettleInvstPosition(CThostFtdcRiskSettleInvstPositionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryRiskSettleProductStatus(CThostFtdcRiskSettleProductStatusField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySPBMFutureParameter(CThostFtdcSPBMFutureParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySPBMOptionParameter(CThostFtdcSPBMOptionParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySPBMIntraParameter(CThostFtdcSPBMIntraParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySPBMInterParameter(CThostFtdcSPBMInterParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySPBMPortfDefinition(CThostFtdcSPBMPortfDefinitionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySPBMInvestorPortfDef(CThostFtdcSPBMInvestorPortfDefField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestorPortfMarginRatio(CThostFtdcInvestorPortfMarginRatioField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestorProdSPBMDetail(CThostFtdcInvestorProdSPBMDetailField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestorCommoditySPMMMargin(CThostFtdcInvestorCommoditySPMMMarginField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestorCommodityGroupSPMMMargin(CThostFtdcInvestorCommodityGroupSPMMMarginField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySPMMInstParam(CThostFtdcSPMMInstParamField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySPMMProductParam(CThostFtdcSPMMProductParamField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQrySPBMAddOnInterParameter(CThostFtdcSPBMAddOnInterParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryRCAMSCombProductInfo(CThostFtdcRCAMSCombProductInfoField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryRCAMSInstrParameter(CThostFtdcRCAMSInstrParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryRCAMSIntraParameter(CThostFtdcRCAMSIntraParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryRCAMSInterParameter(CThostFtdcRCAMSInterParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryRCAMSShortOptAdjustParam(CThostFtdcRCAMSShortOptAdjustParamField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryRCAMSInvestorCombPosition(CThostFtdcRCAMSInvestorCombPositionField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestorProdRCAMSMargin(CThostFtdcInvestorProdRCAMSMarginField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryRULEInstrParameter(CThostFtdcRULEInstrParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryRULEIntraParameter(CThostFtdcRULEIntraParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryRULEInterParameter(CThostFtdcRULEInterParameterField *p, CThostFtdcRspInfoField *e, int id, bool last) override;
  void OnRspQryInvestorProdRULEMargin(CThostFtdcInvestorProdRULEMarginField *p, CThostFtdcRspInfoField *e, int id, bool last) override;

private:
  CThostFtdcTraderApi *api_;
  EventChannel *ch_;
};
} // namespace ctp
#endif
