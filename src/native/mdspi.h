/*
 * mdspi.h - market-data SPI. Each CTP callback just pushes a binary record into
 * the EventChannel (no JS, no object building) on the CTP callback thread.
 *
 * Event codes are mirrored in src/market-data.ts (MD_BASE + n).
 */

#ifndef CTP_NATIVE_MDSPI_H
#define CTP_NATIVE_MDSPI_H

#include "ThostFtdcMdApi.h"
#include "channel.h"

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
  MdSpi(CThostFtdcMdApi *api, EventChannel *channel) : api_(api), ch_(channel) {}
  ~MdSpi() {}

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
  CThostFtdcMdApi *api_;
  EventChannel *ch_;
};

} // namespace ctp

#endif /* CTP_NATIVE_MDSPI_H */
