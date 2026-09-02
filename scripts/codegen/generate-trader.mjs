/*
 * generate-trader.mjs - generate the whole trader binding from the header.
 *
 * Emits (src/generated/):
 *   traderspi.gen.h/.cc - TraderSpi: 150+ callbacks, each pushes a record
 *   traderreq.gen.h/.cc - traderReq(): methodId switch -> ReqXxx (risk-gated
 *                         on order insert)
 *   trader.gen.ts       - TraderEvent enum, TRADER_EVENTS map, TraderBase with
 *                         a typed reqXxx() per request method
 */

import { writeFile } from "node:fs/promises";
import { parseTraderApi } from "./parse-traderapi.mjs";
import { splitWords, camelCase } from "./naming.mjs";

const ROOT = new URL("../../", import.meta.url);
const HEADER = new URL("ctpsdk/ThostFtdcTraderApi.h", ROOT);
const OUT = new URL("src/generated/", ROOT);

const { spi: spiAll, req: reqAll } = await parseTraderApi(HEADER);

// Overloads/duplicate declarations cannot be represented by the generated
// name-based JS surface. Fail an SDK upgrade explicitly instead of silently
// keeping the first declaration.
function rejectDuplicateNames(items, label) {
  const seen = new Set();
  for (const item of items) {
    if (seen.has(item.name))
      throw new Error(`duplicate ${label} declaration: ${item.name}`);
    seen.add(item.name);
  }
  return items;
}
const spi = rejectDuplicateNames(spiAll, "Trader SPI");
const req = rejectDuplicateNames(reqAll, "Trader request");

const kebab = (method) => splitWords(method.replace(/^On/, "")).map((w) => w.toLowerCase()).join("-");
const member = (method) => method.replace(/^On/, "");

const ET_BASE = 0x2000;

// ---------- traderspi.gen.h ----------
let h = `/* AUTO-GENERATED. Do not edit. */
#ifndef CTP_TRADERSPI_GEN_H
#define CTP_TRADERSPI_GEN_H
#include "ThostFtdcTraderApi.h"
#include "../native/arm.h"
#include "../native/channel.h"
#include "../native/risk.h"
namespace ctp {
enum {
  ET_BASE = ${ET_BASE},
`;
spi.forEach((s, i) => (h += `  ET_${member(s.name)} = ET_BASE + ${i + 1},\n`));
h += `};

class TraderSpi : public CThostFtdcTraderSpi {
public:
  explicit TraderSpi(EventChannel *channel) : ch_(channel) {}
  ~TraderSpi() {}

  void setRisk(RiskEngine *r) { risk_ = r; }
  void setArmRegistry(ArmRegistry *a) { arm_ = a; }

`;
for (const s of spi) h += `  void ${s.name}(${rawDecl(s)}) override;\n`;
h += `
private:
  EventChannel *ch_;
  RiskEngine *risk_ = nullptr;
  ArmRegistry *arm_ = nullptr;
};
} // namespace ctp
#endif
`;

// Reconstruct an exact (type-matching) parameter list with canonical names.
function rawDecl(s) {
  if (s.intArg) return "int arg";
  const parts = [];
  if (s.struct) parts.push(`CThostFtdc${s.struct}Field *p`);
  if (s.hasRsp) parts.push("CThostFtdcRspInfoField *e");
  if (s.hasReqId) parts.push("int id");
  if (s.hasIsLast) parts.push("bool last");
  return parts.join(", ");
}

// ---------- traderspi.gen.cc ----------
let c = `/* AUTO-GENERATED. Do not edit. */
#include "traderspi.gen.h"
#include "structids.gen.h"
#include <string>

namespace ctp {

static int eid(CThostFtdcRspInfoField *e) { return e ? e->ErrorID : 0; }
static const char *emsg(CThostFtdcRspInfoField *e) { return (e && e->ErrorID) ? e->ErrorMsg : ""; }

static bool validSingleOffset(const char *flags, size_t capacity) {
  return capacity > 0 && flags[0] >= '0' && flags[0] <= '6' &&
         (capacity == 1 || flags[1] == '\\0');
}

static bool validHedgeFlag(char flag) {
  return flag == '1' || flag == '2' || flag == '3' || flag == '5' ||
         flag == '6' || flag == '7';
}

static bool validSingleHedge(const char *flags, size_t capacity) {
  return capacity > 0 && validHedgeFlag(flags[0]) &&
         (capacity == 1 || flags[1] == '\\0');
}

template <size_t N>
static bool boundedField(const char (&field)[N], std::string &out) {
  size_t length = 0;
  while (length < N && field[length] != '\\0')
    ++length;
  if (length == N)
    return false;
  out.assign(field, length);
  return true;
}

`;

// These callbacks prove that account state changed through a path the ordinary
// single-leg order/position ledger cannot model. CTP broadcasts many realtime
// returns across an investor's sessions, so this covers activity initiated by
// another terminal as well as requests sent by this process. Keep the list
// explicit: an SDK rename/removal must force a codegen review.
const UNSUPPORTED_RISK_RETURNS = new Set([
  "OnRtnExecOrder", "OnRtnQuote", "OnRtnOptionSelfClose", "OnRtnCombAction",
  "OnRtnOffsetSetting", "OnRtnSpdApply", "OnRtnHedgeCfm",
  "OnRtnFromBankToFutureByBank", "OnRtnFromFutureToBankByBank",
  "OnRtnRepealFromBankToFutureByBank", "OnRtnRepealFromFutureToBankByBank",
  "OnRtnFromBankToFutureByFuture", "OnRtnFromFutureToBankByFuture",
  "OnRtnRepealFromBankToFutureByFutureManual",
  "OnRtnRepealFromFutureToBankByFutureManual",
  "OnRtnRepealFromBankToFutureByFuture",
  "OnRtnRepealFromFutureToBankByFuture",
]);
for (const name of UNSUPPORTED_RISK_RETURNS) {
  if (!spi.some((callback) => callback.name === name))
    throw new Error(`required unsupported-risk callback is absent: ${name}`);
}

// Every realtime OnRtn* return must be EXPLICITLY classified, mirroring the
// request-side classifyRequest() guarantee. Otherwise a future SDK that adds a
// new position-changing return would fall through to a plain event push with no
// risk handling — a silent fail-open. A return is either risk-wired
// (OnRtnTrade/OnRtnOrder feed the ledger), an unsupported mutation that taints
// it (UNSUPPORTED_RISK_RETURNS), or informational / non-position-changing
// (below: bulletins, notices, instrument status, RFQ response, bank ACCOUNT
// registration + balance queries, conditional-order errors). An unclassified
// OnRtn* throws here and forces a codegen review before it can ship.
const RISK_WIRED_RTN_RETURNS = new Set(["OnRtnTrade", "OnRtnOrder"]);
const INFORMATIONAL_RTN_RETURNS = new Set([
  "OnRtnBulletin", "OnRtnCFMMCTradingAccountToken", "OnRtnCancelAccountByBank",
  "OnRtnChangeAccountByBank", "OnRtnErrorConditionalOrder", "OnRtnForQuoteRsp",
  "OnRtnInstrumentStatus", "OnRtnOpenAccountByBank", "OnRtnPrivateSeqNo",
  "OnRtnQueryBankBalanceByFuture", "OnRtnTradingNotice",
]);
for (const callback of spi) {
  if (!callback.name.startsWith("OnRtn")) continue;
  if (RISK_WIRED_RTN_RETURNS.has(callback.name) ||
      UNSUPPORTED_RISK_RETURNS.has(callback.name) ||
      INFORMATIONAL_RTN_RETURNS.has(callback.name))
    continue;
  throw new Error(
    `unclassified OnRtn* callback '${callback.name}': classify it as risk-wired, ` +
    `unsupported-mutation, or informational before codegen — otherwise it emits ` +
    `a plain event push with no risk handling (a silent fail-open).`
  );
}

for (const s of spi) {
  const reqId = s.hasReqId ? "id" : "0";
  const isLast = s.hasIsLast ? "last ? 1 : 0" : "-1";
  const errId = s.hasRsp ? "eid(e)" : "0";
  const errMsg = s.hasRsp ? "emsg(e)" : '""';
  let sid, ptr, len;
  if (s.struct) {
    sid = `p ? SID_${s.struct} : -1`;
    ptr = "p";
    len = "p ? (int)sizeof(*p) : 0";
  } else if (s.intArg) {
    sid = "-2";
    ptr = "&arg";
    len = "sizeof(arg)";
  } else {
    sid = "-1";
    ptr = "nullptr";
    len = "0";
  }
  c += `void TraderSpi::${s.name}(${rawDecl(s)}) {\n`;
  if (s.name === "OnFrontConnected" || s.name === "OnFrontDisconnected") {
    // ArmRegistry::onTick takes the arm lock before entering the RiskEngine.
    // Preserve that lock order here: clear mandates first, then close the
    // session ledger. During a front transition CTP cannot accept a normal
    // order before the new login completes.
    c += `  if (arm_)\n    arm_->clear();\n  if (risk_)\n    risk_->invalidateSession();\n`;
  } else if (s.name === "OnRtnTrade") {
    c += `  if (p && risk_) {\n    std::string tradingDay, tradeDate, broker, investor, exchange, instrument, tradeId, orderSysId, orderRef;\n    const bool stringsValid = boundedField(p->TradingDay, tradingDay) && boundedField(p->TradeDate, tradeDate) && boundedField(p->BrokerID, broker) && boundedField(p->InvestorID, investor) && boundedField(p->ExchangeID, exchange) && boundedField(p->InstrumentID, instrument) && boundedField(p->TradeID, tradeId) && boundedField(p->OrderSysID, orderSysId) && boundedField(p->OrderRef, orderRef);\n    if (stringsValid && !instrument.empty() && !broker.empty() && !investor.empty() && (p->Direction == '0' || p->Direction == '1') && p->OffsetFlag >= '0' && p->OffsetFlag <= '6' && validHedgeFlag(p->HedgeFlag)) {\n      const std::string &day = !tradingDay.empty() ? tradingDay : tradeDate;\n      std::string key;\n      if (!day.empty() && !exchange.empty() && !tradeId.empty())\n        key = day + "|" + broker + "|" + investor + "|" + exchange + "|" + instrument + "|trade|" + tradeId;\n      else if (!day.empty() && !exchange.empty() && p->SequenceNo != 0)\n        key = day + "|" + broker + "|" + investor + "|" + exchange + "|" + instrument + "|sequence|" + std::to_string(p->SequenceNo);\n      const std::string sys = !tradingDay.empty() && !exchange.empty() && !orderSysId.empty() ? tradingDay + "|" + broker + "|" + investor + "|" + exchange + "|" + instrument + "|" + orderSysId : std::string();\n      risk_->onTrade(key, orderRef, sys, instrument, p->Direction == '0', p->OffsetFlag == '0', p->HedgeFlag, p->Price, p->Volume, broker, investor);\n    } else {\n      risk_->markTradeLedgerUncertain();\n    }\n  }\n`;
  } else if (s.name === "OnRtnOrder") {
    // reconcile the order's in-flight reservation to its working remainder;
    // keyed by the originating (FrontID, SessionID, OrderRef)
    c += `  if (p && risk_) {\n    std::string day, broker, investor, exchange, instrument, orderSysId, orderRef;\n    const bool stringsValid = boundedField(p->TradingDay, day) && boundedField(p->BrokerID, broker) && boundedField(p->InvestorID, investor) && boundedField(p->ExchangeID, exchange) && boundedField(p->InstrumentID, instrument) && boundedField(p->OrderSysID, orderSysId) && boundedField(p->OrderRef, orderRef);\n    const std::string sys = stringsValid && !day.empty() && !broker.empty() && !investor.empty() && !exchange.empty() && !instrument.empty() && !orderSysId.empty() ? day + "|" + broker + "|" + investor + "|" + exchange + "|" + instrument + "|" + orderSysId : std::string();\n    if (stringsValid && !broker.empty() && !investor.empty() && validSingleOffset(p->CombOffsetFlag, sizeof(p->CombOffsetFlag)) && validSingleHedge(p->CombHedgeFlag, sizeof(p->CombHedgeFlag)) && (p->Direction == '0' || p->Direction == '1') && !instrument.empty() && !orderRef.empty()) {\n      risk_->onOrderUpdate(p->FrontID, p->SessionID, orderRef, instrument, p->CombOffsetFlag[0] == '0', p->Direction == '0', p->CombHedgeFlag[0], p->OrderStatus, p->LimitPrice, p->VolumeTotalOriginal, p->VolumeTraded, sys, broker, investor);\n    } else {\n      risk_->onUnsupportedOrderUpdate(p->FrontID, p->SessionID, orderRef, p->OrderStatus, sys, broker, investor);\n    }\n  }\n`;
  } else if (s.name === "OnRspOrderInsert" || s.name === "OnErrRtnOrderInsert") {
    // Only an ordinary single-leg OPEN owns a reservation. OnRsp carries the
    // exact request id; OnErrRtn does not, so the latter must conservatively
    // make a matching ledger uncertain instead of releasing by a reusable ref.
    const correlated = s.name === "OnRspOrderInsert";
    // A valid close row owns no open reservation and is safely ignored. A null
    // or malformed row is ambiguous and therefore enters the conservative
    // correlation path (requestId can still identify an open exactly).
    const rejectionGuard = correlated ? "eid(e) != 0 && " : "";
    c += `  if (risk_ && ${rejectionGuard}(!p || !validSingleOffset(p->CombOffsetFlag, sizeof(p->CombOffsetFlag)) || p->CombOffsetFlag[0] == '0')) {\n    std::string orderRef, broker, investor;\n    if (p) { boundedField(p->OrderRef, orderRef); boundedField(p->BrokerID, broker); boundedField(p->InvestorID, investor); }\n    risk_->${correlated ? "rejectInsertByRequest(id, orderRef, broker, investor)" : "rejectInsertUnscoped(orderRef, broker, investor)"};\n  }\n`;
  } else if (s.name === "OnRspUserLogin") {
    // Publish only the newest explicitly requested login. A delayed success
    // from a superseded login/logout must not restore an obsolete session key.
    c += `  if (risk_ && last) {\n    if (!p || eid(e) != 0 || !risk_->acceptLoginSession(id, p->FrontID, p->SessionID, p->BrokerID, p->UserID))\n      risk_->finishSessionTransition(id);\n  }\n`;
  } else if (s.name === "OnRspUserLogout") {
    c += `  if (risk_ && last)\n    risk_->finishSessionTransition(id);\n`;
  } else if (UNSUPPORTED_RISK_RETURNS.has(s.name)) {
    c += `  if (p && risk_)\n    risk_->recordUnsupportedRiskMutation();\n`;
  } else if (s.name === "OnRspQryInstrumentMarginRate") {
    // Preserve every dimension needed by CTP's futures-margin formula. Relative
    // schedules need an exchange base rate and therefore remain fail-closed.
    c += `  if (p && risk_ && eid(e) == 0) {\n    std::string broker, investor, investUnit, instrument;\n    if (boundedField(p->BrokerID, broker) && boundedField(p->InvestorID, investor) && boundedField(p->InvestUnitID, investUnit) && boundedField(p->InstrumentID, instrument))\n      risk_->applyInvestorMarginSchedule(id, broker, investor, investUnit, instrument, p->HedgeFlag, p->LongMarginRatioByMoney, p->LongMarginRatioByVolume, p->ShortMarginRatioByMoney, p->ShortMarginRatioByVolume, p->IsRelative != 0);\n  }\n  if (risk_ && last) risk_->finishMarginScheduleQuery(id, eid(e) == 0);\n`;
  } else if (s.name === "OnRspQryExchangeMarginRate") {
    // Base schedule used when the investor schedule is marked IsRelative.
    c += `  if (p && risk_ && eid(e) == 0) {\n    std::string broker, instrument;\n    if (boundedField(p->BrokerID, broker) && boundedField(p->InstrumentID, instrument))\n      risk_->applyExchangeMarginSchedule(id, broker, instrument, p->HedgeFlag, p->LongMarginRatioByMoney, p->LongMarginRatioByVolume, p->ShortMarginRatioByMoney, p->ShortMarginRatioByVolume);\n  }\n  if (risk_ && last) risk_->finishMarginScheduleQuery(id, eid(e) == 0);\n`;
  } else if (s.name === "OnRspQryInstrument") {
    // feed the contract multiplier straight into the risk engine (no JS round-
    // trip) - idempotent metadata, same pattern as the margin rate above.
    c += `  if (p && risk_ && eid(e) == 0) {\n    std::string instrument;\n    if (boundedField(p->InstrumentID, instrument))\n      risk_->setInstrument(instrument, p->VolumeMultiple, p->ProductClass);\n    else\n      risk_->invalidateInstrument("");\n  }\n`;
  }
  c += `  ch_->push(ET_${member(s.name)}, ${reqId}, ${isLast}, ${errId}, ${errMsg}, ${sid}, ${ptr}, ${len});\n`;
  c += `}\n\n`;
}
c += `} // namespace ctp\n`;

// ---------- traderreq.gen.h ----------
let rh = `/* AUTO-GENERATED. Do not edit. */
#ifndef CTP_TRADERREQ_GEN_H
#define CTP_TRADERREQ_GEN_H
#include "ThostFtdcTraderApi.h"
#define CTP_RISK_BLOCKED -10001
#define CTP_RATE_LIMITED -10002
#define CTP_POSITION_LIMIT -10003
#define CTP_POSITION_VOLUME_LIMIT -10004
#define CTP_DUPLICATE_ORDER_REF -10005
#define CTP_UNSUPPORTED_RISK_PATH -10006
#define CTP_RISK_METADATA_UNAVAILABLE -10007
#define CTP_INVALID_ORDER_FIELDS -10008
namespace ctp {
class RiskEngine;
enum {
`;
req.forEach((r, i) => (rh += `  M_${r.name} = ${i + 1},\n`));
rh += `};
int traderReq(CThostFtdcTraderApi *api, int methodId, const void *bytes, int len,
              int requestId, RiskEngine *risk);
} // namespace ctp
#endif
`;

// ---------- traderreq.gen.cc ----------
let rc = `/* AUTO-GENERATED. Do not edit. */
#include "traderreq.gen.h"
#include "ThostFtdcUserApiStruct.h"
#include "../native/order_capability.h"
#include "../native/risk.h"
#include <algorithm>
#include <cstring>

namespace ctp {

static bool validOffsetFlags(const char *flags, size_t capacity) {
  return capacity > 0 && flags[0] >= '0' && flags[0] <= '6' &&
         (capacity == 1 || flags[1] == '\\0');
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
         (capacity == 1 || flags[1] == '\\0');
}

int traderReq(CThostFtdcTraderApi *api, int methodId, const void *bytes, int len,
              int requestId, RiskEngine *risk) {
  switch (methodId) {
`;
// Every non-query request must be classified here. Queries are read-only by CTP
// naming convention; all other future SDK additions fail generation until their
// risk semantics are reviewed. This prevents a new mutation whose name does not
// end in "Insert" from silently bypassing the local gate.
const UNSUPPORTED_RISK_MUTATIONS = new Set([
  "ReqOffsetSetting", "ReqSpdApply", "ReqHedgeCfm",
]);
const FUNDS_TRANSFERS = new Set([
  "ReqFromBankToFutureByFuture", "ReqFromFutureToBankByFuture",
]);
const SAFE_NON_QUERY_REQUESTS = new Set([
  "ReqAuthenticate", "ReqUserLogin", "ReqUserLogout",
  "ReqUserPasswordUpdate", "ReqTradingAccountPasswordUpdate",
  "ReqUserAuthMethod", "ReqGenUserCaptcha", "ReqGenUserText",
  "ReqUserLoginWithCaptcha", "ReqUserLoginWithText", "ReqUserLoginWithOTP",
  "ReqGenSMSCode", "ReqSettlementInfoConfirm",
  // Cancellation/removal paths remain available in every risk mode.
  "ReqRemoveParkedOrder", "ReqRemoveParkedOrderAction", "ReqBatchOrderAction",
  "ReqCancelOffsetSetting", "ReqSpdApplyAction", "ReqHedgeCfmAction",
]);
const SESSION_TRANSITION_REQUESTS = new Set([
  "ReqUserLogin", "ReqUserLogout", "ReqUserLoginWithCaptcha",
  "ReqUserLoginWithText", "ReqUserLoginWithOTP",
]);
for (const name of SESSION_TRANSITION_REQUESTS) {
  if (!req.some((request) => request.name === name))
    throw new Error(`required session-transition request is absent: ${name}`);
}
// These methods accept ActionFlag=Delete ('0') or Modify ('3'). Only deletion
// is unconditionally risk-reducing; modification can increase quantity or move
// price and therefore uses the unsupported-mutation gate.
const CONDITIONAL_ACTION_REQUESTS = new Set([
  "ReqParkedOrderAction", "ReqOrderAction", "ReqExecOrderAction",
  "ReqQuoteAction", "ReqOptionSelfCloseAction",
]);
function classifyRequest(name) {
  if (name === "ReqOrderInsert") return "order";
  if (FUNDS_TRANSFERS.has(name)) return "funds";
  if (CONDITIONAL_ACTION_REQUESTS.has(name)) return "conditional-action";
  if (name.endsWith("Insert") || UNSUPPORTED_RISK_MUTATIONS.has(name))
    return "unsupported-risk-mutation";
  if (name.startsWith("ReqQry") || name.startsWith("ReqQuery") ||
      SAFE_NON_QUERY_REQUESTS.has(name))
    return "safe";
  throw new Error(`unclassified non-query Trader request: ${name}`);
}
for (const r of req) {
  const classification = classifyRequest(r.name);
  const T = `CThostFtdc${r.struct}Field`;
  rc += `  case M_${r.name}: {\n`;
  rc += `    ${T} f;\n    std::memset(&f, 0, sizeof(f));\n`;
  rc += `    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));\n`;
  // A relogin/logout can change identity without a front disconnect. Record
  // the exact newest request id before send; stale responses are ignored.
  if (SESSION_TRANSITION_REQUESTS.has(r.name))
    rc += `    if (risk) risk->beginSessionTransition(requestId, ${r.name !== "ReqUserLogout" ? "true" : "false"}, f.BrokerID, f.UserID);\n`;
  if (r.name === "ReqQryInstrument")
    rc += `    if (risk) risk->invalidateInstrument(f.InstrumentID);\n`;
  else if (r.name === "ReqQryInstrumentMarginRate")
    rc += `    if (risk) risk->beginMarginScheduleQuery(requestId, false, f.InstrumentID, f.HedgeFlag, f.BrokerID, f.InvestorID);\n`;
  else if (r.name === "ReqQryExchangeMarginRate")
    rc += `    if (risk) risk->beginMarginScheduleQuery(requestId, true, f.InstrumentID, f.HedgeFlag, f.BrokerID, "");\n`;
  if (classification === "order") {
    rc += `    if (risk) {\n`;
    rc += `      OrdinaryOrderIdentity identity;\n`;
    rc += `      if (!decodeOrdinaryOrder(f, identity)) return CTP_INVALID_ORDER_FIELDS;\n`;
    rc += `      bool isOpen = hasOpenOffset(f.CombOffsetFlag, sizeof(f.CombOffsetFlag));\n`;
    rc += `      bool sendAllowed = false;\n`;
    rc += `      auto sendGuard = risk->acquireOrderSendForAccount(isOpen, identity.brokerId, identity.investorId, sendAllowed);\n`;
    rc += `      if (!sendAllowed) return CTP_RISK_BLOCKED;\n`;
    rc += `      RiskVerdict v = risk->check(identity.instrumentId, f.LimitPrice, risk->refPrice(identity.instrumentId), f.VolumeTotalOriginal, isOpen);\n`;
    rc += `      if (!v.ok) return CTP_RISK_BLOCKED;\n`;
    rc += `      if (isOpen) {\n`;
    rc += `        OpenGate g = risk->tryReserveOpen(identity.orderRef, identity.instrumentId, f.Direction == '0', f.LimitPrice, f.VolumeTotalOriginal, f.CombHedgeFlag[0]);\n`;
    rc += `        if (g == OpenGate::VolumeLimit) return CTP_POSITION_VOLUME_LIMIT;\n`;
    rc += `        if (g == OpenGate::CostLimit) return CTP_POSITION_LIMIT;\n`;
    rc += `        if (g == OpenGate::DuplicateOrderRef) return CTP_DUPLICATE_ORDER_REF;\n`;
    rc += `        if (g == OpenGate::MetadataUnavailable) return CTP_RISK_METADATA_UNAVAILABLE;\n`;
    rc += `        if (!risk->bindInsertRequest(requestId, identity.orderRef)) return CTP_RISK_METADATA_UNAVAILABLE;\n`;
    rc += `      }\n`;
    rc += `      if (isOpen && !risk->allowRate()) {\n`;
    rc += `        risk->rollbackInsertRequest(requestId, identity.orderRef);\n`;
    rc += `        return CTP_RATE_LIMITED;\n`;
    rc += `      }\n`;
    rc += `      if (!isOpen && !risk->allowCloseRate()) return CTP_RATE_LIMITED;\n`;
    rc += `      int sendRc = api->ReqOrderInsert(&f, requestId);\n`;
    rc += `      if (sendRc != 0 && isOpen) risk->rollbackInsertRequest(requestId, identity.orderRef); // request not accepted; safe to retry this ref\n`;
    rc += `      return sendRc;\n`;
    rc += `    }\n`;
  } else if (classification === "unsupported-risk-mutation") {
    rc += `    if (risk) {\n`;
    rc += `      bool sendAllowed = false;\n`;
    rc += `      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);\n`;
    rc += `      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;\n`;
    rc += `      int sendRc = api->${r.name}(&f, requestId);\n`;
    rc += `      if (sendRc == 0) risk->recordUnsupportedRiskMutation();\n`;
    rc += `      return sendRc;\n`;
    rc += `    }\n`;
  } else if (classification === "funds") {
    rc += `    if (risk) {\n`;
    rc += `      bool sendAllowed = false;\n`;
    rc += `      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);\n`;
    // Funds transfers change available collateral but are not represented by
    // the local ledger. Forward them only in protocol-binding mode, exactly
    // like every other unsupported risk mutation.
    rc += `      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;\n`;
    rc += `      int sendRc = api->${r.name}(&f, requestId);\n`;
    rc += `      if (sendRc == 0) risk->recordUnsupportedRiskMutation();\n`;
    rc += `      return sendRc;\n`;
    rc += `    }\n`;
  } else if (classification === "conditional-action") {
    rc += `    if (risk && f.ActionFlag != '0') {\n`;
    rc += `      bool sendAllowed = false;\n`;
    rc += `      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);\n`;
    rc += `      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;\n`;
    rc += `      int sendRc = api->${r.name}(&f, requestId);\n`;
    rc += `      if (sendRc == 0) risk->recordUnsupportedRiskMutation();\n`;
    rc += `      return sendRc;\n`;
    rc += `    }\n`;
  }
  if (SESSION_TRANSITION_REQUESTS.has(r.name)) {
    rc += `    int sendRc = api->${r.name}(&f, requestId);\n`;
    rc += `    if (sendRc != 0 && risk) risk->finishSessionTransition(requestId);\n`;
    rc += `    return sendRc;\n  }\n`;
  } else {
    if (r.name === "ReqQryInstrumentMarginRate" || r.name === "ReqQryExchangeMarginRate") {
      rc += `    int sendRc = api->${r.name}(&f, requestId);\n`;
      rc += `    if (sendRc != 0 && risk) risk->finishMarginScheduleQuery(requestId, false);\n`;
      rc += `    return sendRc;\n  }\n`;
    } else {
      rc += `    return api->${r.name}(&f, requestId);\n  }\n`;
    }
  }
}
rc += `  default:
    return -1;
  }
}

} // namespace ctp
`;

// ---------- trader.gen.ts ----------
let ts = `/* AUTO-GENERATED by scripts/codegen/generate-trader.mjs. Do not edit. */

import { CtpClient } from "../client.js";
import { copyOwnRecord } from "../validation.js";
import { STRUCT_ID } from "./structs.gen.js";
import type * as S from "./structs.gen.js";

export enum TraderEvent {
`;
for (const s of spi) ts += `  ${member(s.name)} = ${JSON.stringify(kebab(s.name))},\n`;
ts += `}

export const TRADER_EVENTS: Map<number, string> = new Map([
`;
spi.forEach((s, i) => (ts += `  [${ET_BASE} + ${i + 1}, ${JSON.stringify(kebab(s.name))}],\n`));
ts += `]);

/* eslint-disable @typescript-eslint/no-explicit-any */
export abstract class TraderBase extends CtpClient {
`;
// Exchange-bound inserts/actions get no success response (only OnRtn*), so they
// resolve on submission via submit(); everything else waits for its OnRsp*.
const FIRE = new Set([
  "ReqOrderInsert", "ReqOrderAction", "ReqExecOrderInsert", "ReqExecOrderAction",
  "ReqForQuoteInsert", "ReqQuoteInsert", "ReqQuoteAction", "ReqBatchOrderAction",
  "ReqOptionSelfCloseInsert", "ReqOptionSelfCloseAction", "ReqCombActionInsert",
]);
req.forEach((r, i) => {
  const method = camelCase(r.name);
  const enc = `this.encode(STRUCT_ID.${r.struct}, req as Record<string, unknown>)`;
  if (FIRE.has(r.name)) {
    ts += `  ${method}(req: Partial<S.${r.struct}> = {}): Promise<void> {\n`;
    ts += `    return this.submit((id) => this.native._req(${i + 1}, ${enc}, id));\n`;
    ts += `  }\n`;
  } else {
    const multi = r.name.startsWith("ReqQry");
    const query = multi || r.name.startsWith("ReqQuery");
    const sessionTransition = SESSION_TRANSITION_REQUESTS.has(r.name);
    ts += `  ${method}(req: Partial<S.${r.struct}> = {}): Promise<${multi ? "unknown[]" : "unknown"}> {\n`;
    if (query || sessionTransition) {
      // Queries may wait behind another in-flight query for seconds. Encode at
      // invocation so caller mutation/accessors cannot alter the eventual wire
      // request. Session transitions encode first, then invalidate old work, so
      // a malformed login/logout attempt does not tear down a healthy context.
      if (sessionTransition) {
        ts += `    let cleanReq: Record<string, unknown>;\n`;
        ts += `    let encoded: Uint8Array;\n`;
        ts += `    try { cleanReq = copyOwnRecord(req, ${JSON.stringify(camelCase(r.name) + " request")}); encoded = this.encode(STRUCT_ID.${r.struct}, cleanReq); } catch (error) { return Promise.reject(error); }\n`;
      } else {
        ts += `    let encoded: Uint8Array;\n`;
        ts += `    try { encoded = ${enc}; } catch (error) { return Promise.reject(error); }\n`;
      }
      if (sessionTransition)
        ts += `    this.beginRequestContextTransition("trader session transition started");\n`;
      if (query) {
        ts += `    return this.queryRequest((id) => this.native._req(${i + 1}, encoded, id), ${multi ? "false" : "true"});\n`;
      } else if (r.name !== "ReqUserLogout") {
        ts += `    return this.request((id) => this.native._req(${i + 1}, encoded, id), true, undefined, true).then((value) => {\n`;
        ts += `      const row = value as { brokerId?: unknown; userId?: unknown; frontId?: unknown; sessionId?: unknown } | undefined;\n`;
        ts += `      if (!row || typeof row.brokerId !== "string" || !row.brokerId || typeof row.userId !== "string" || !row.userId || !Number.isInteger(row.frontId) || Number(row.frontId) <= 0 || !Number.isInteger(row.sessionId) || Number(row.sessionId) === 0 || (typeof cleanReq.brokerId === "string" && cleanReq.brokerId && row.brokerId !== cleanReq.brokerId) || (typeof cleanReq.userId === "string" && cleanReq.userId && row.userId !== cleanReq.userId)) throw new Error("trader login response has missing or mismatched session identity");\n`;
        ts += `      return value;\n    });\n`;
      } else {
        ts += `    return this.request((id) => this.native._req(${i + 1}, encoded, id), true, undefined, true);\n`;
      }
    } else {
      ts += `    return this.request((id) => this.native._req(${i + 1}, ${enc}, id), true);\n`;
    }
    ts += `  }\n`;
  }
});
ts += `}\n`;

await writeFile(new URL("traderspi.gen.h", OUT), h);
await writeFile(new URL("traderspi.gen.cc", OUT), c);
await writeFile(new URL("traderreq.gen.h", OUT), rh);
await writeFile(new URL("traderreq.gen.cc", OUT), rc);
await writeFile(new URL("trader.gen.ts", OUT), ts);

console.log(`generated trader: ${spi.length} SPI callbacks, ${req.length} req methods -> src/generated/`);
