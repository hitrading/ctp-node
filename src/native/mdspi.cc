/*
 * mdspi.cc - see mdspi.h
 */

#include "mdspi.h"

#include <cstdio>

#include "../generated/structids.gen.h"

namespace ctp {

static int eid(CThostFtdcRspInfoField *e) { return e ? e->ErrorID : 0; }
static const char *emsg(CThostFtdcRspInfoField *e) {
  return (e && e->ErrorID) ? e->ErrorMsg : "";
}

void MdSpi::OnFrontConnected() {
  ch_->push(MD_FRONT_CONNECTED, 0, -1, 0, "", -1, nullptr, 0);
}

void MdSpi::OnFrontDisconnected(int nReason) {
  int r = nReason;
  ch_->push(MD_FRONT_DISCONNECTED, 0, -1, 0, "", -2, &r, sizeof(r));
}

void MdSpi::OnHeartBeatWarning(int nTimeLapse) {
  int t = nTimeLapse;
  ch_->push(MD_HEARTBEAT_WARNING, 0, -1, 0, "", -2, &t, sizeof(t));
}

void MdSpi::OnRspUserLogin(CThostFtdcRspUserLoginField *p,
                           CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(MD_RSP_USER_LOGIN, id, last ? 1 : 0, eid(e), emsg(e),
            p ? SID_RspUserLogin : -1, p, p ? (int)sizeof(*p) : 0);
}

void MdSpi::OnRspUserLogout(CThostFtdcUserLogoutField *p,
                            CThostFtdcRspInfoField *e, int id, bool last) {
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
  ch_->push(MD_RSP_ERROR, id, last ? 1 : 0, eid(e), emsg(e), -1, nullptr, 0);
}

void MdSpi::OnRspSubMarketData(CThostFtdcSpecificInstrumentField *p,
                               CThostFtdcRspInfoField *e, int id, bool last) {
  ch_->push(MD_RSP_SUB_MARKET_DATA, id, last ? 1 : 0, eid(e), emsg(e),
            p ? SID_SpecificInstrument : -1, p, p ? (int)sizeof(*p) : 0);
}

void MdSpi::OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField *p,
                                 CThostFtdcRspInfoField *e, int id, bool last) {
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
  // Load the api once: a concurrent close() (clearApi) must not let us deref a
  // freed api between the null-check and the call.
  CThostFtdcMdApi *api = api_.load(std::memory_order_relaxed);
  if (p && api) {
    const char *td = api->GetTradingDay();
    if (td)
      std::snprintf(p->TradingDay, sizeof(p->TradingDay), "%s", td);
  }
  // Update the latest-tick cache, evaluate armed triggers (lowest tick-to-order
  // latency), then publish to the ring.
  if (p) {
    SnapshotCache *sc = snap_.load(std::memory_order_relaxed);
    if (sc)
      sc->update(*p); // LVC: latest full tick per instrument (incl. trading day)
    ArmRegistry *ar = armReg_.load(std::memory_order_relaxed);
    if (ar)
      ar->onTick(p->InstrumentID, p->BidPrice1, p->AskPrice1);
  }
  ch_->push(MD_RTN_DEPTH_MARKET_DATA, 0, -1, 0, "", SID_DepthMarketData, p,
            p ? (int)sizeof(*p) : 0);
}

void MdSpi::OnRtnForQuoteRsp(CThostFtdcForQuoteRspField *p) {
  ch_->push(MD_RTN_FOR_QUOTE, 0, -1, 0, "", SID_ForQuoteRsp, p,
            p ? (int)sizeof(*p) : 0);
}

} // namespace ctp
