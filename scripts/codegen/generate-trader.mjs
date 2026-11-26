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

// These callbacks carry the session lifecycle and instrument metadata into the
// native risk engine (see the special cases in traderspi.gen.cc below). A
// renamed or removed callback in a future SDK would silently drop that wiring,
// so fail generation instead.
const RISK_WIRED_CALLBACKS = [
  "OnFrontConnected", "OnFrontDisconnected", "OnRspUserLogin",
  "OnRspUserLogout", "OnRspQryInstrument", "OnRspError",
];
for (const name of RISK_WIRED_CALLBACKS) {
  if (!spi.some((callback) => callback.name === name))
    throw new Error(`required risk-wired Trader SPI callback is absent: ${name}`);
}

const kebab = (method) => splitWords(method.replace(/^On/, "")).map((w) => w.toLowerCase()).join("-");
const member = (method) => method.replace(/^On/, "");

const ET_BASE = 0x2000;

// ---------- traderspi.gen.h ----------
let h = `/* AUTO-GENERATED. Do not edit. */
#ifndef CTP_TRADERSPI_GEN_H
#define CTP_TRADERSPI_GEN_H
#include "ThostFtdcTraderApi.h"
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

`;
for (const s of spi) h += `  void ${s.name}(${rawDecl(s)}) override;\n`;
h += `
private:
  EventChannel *ch_;
  RiskEngine *risk_ = nullptr;
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
#include "../native/ctp_field.h"
#include <string>

namespace ctp {

static int eid(CThostFtdcRspInfoField *e) { return e ? e->ErrorID : 0; }
static const char *emsg(CThostFtdcRspInfoField *e) { return (e && e->ErrorID) ? e->ErrorMsg : ""; }

`;

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
    // During a front transition CTP cannot accept a normal order before the
    // new login completes; account-bound sends stay closed until then. An
    // instrument query of the old connection can no longer complete, so its
    // staged rows are discarded.
    c += `  if (risk_) {\n    risk_->invalidateSession();\n    risk_->abandonInstrumentQuery();\n  }\n`;
  } else if (s.name === "OnRspUserLogin") {
    // Publish only the newest explicitly requested login. A delayed success
    // from a superseded login/logout must not restore an obsolete session key.
    c += `  if (risk_ && last) {\n    if (!p || eid(e) != 0 || !risk_->acceptLoginSession(id, p->FrontID, p->SessionID, p->BrokerID, p->UserID))\n      risk_->finishSessionTransition(id);\n  }\n`;
  } else if (s.name === "OnRspUserLogout") {
    c += `  if (risk_ && last)\n    risk_->finishSessionTransition(id);\n`;
  } else if (s.name === "OnRspError") {
    // The generic error callback fails the request it names; an instrument
    // query of that id discards its staged rows (other ids are ignored).
    c += `  if (risk_)\n    risk_->finishInstrumentQuery(id, false);\n`;
  } else if (s.name === "OnRspQryInstrument") {
    // Feed the contract multiplier straight into the risk engine (no JS
    // round-trip) for the maxNotional check. Rows are staged under their
    // request id with their full identity (scope and duplicate checks); only
    // the successful final packet of the newest instrument query publishes
    // them, an error packet discards them.
    c += `  if (risk_) {\n    if (eid(e) != 0) {\n      risk_->finishInstrumentQuery(id, false);\n    } else {\n      if (p) {\n        InstrumentIdentity row;\n        if (readInstrumentIdentity(*p, row))\n          risk_->stageInstrument(id, row, p->VolumeMultiple, p->ProductClass);\n        else\n          risk_->stageUnreadableInstrument(id);\n      }\n      if (last)\n        risk_->finishInstrumentQuery(id, true);\n    }\n  }\n`;
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
#define CTP_UNSUPPORTED_RISK_PATH -10006
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

static bool hasOpenOffset(const char *flags, size_t capacity) {
  for (size_t i = 0; i < capacity && flags[i]; ++i)
    if (flags[i] == '0') return true;
  return false;
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
  // The query's filter is its scope: rows outside it never stage, and a query
  // filtered only by exchange/product/exchange code merges instead of clearing
  // the instruments outside its scope. An unreadable filter clears everything
  // and poisons the query (fail closed).
  if (r.name === "ReqQryInstrument") {
    rc += `    if (risk) {\n      InstrumentIdentity scope;\n      if (readInstrumentIdentity(f, scope)) {\n        risk->beginInstrumentQuery(requestId, scope);\n      } else {\n        risk->beginInstrumentQuery(requestId, InstrumentIdentity{});\n        risk->stageUnreadableInstrument(requestId);\n      }\n    }\n`;
  }
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
    rc += `      if (isOpen ? !risk->allowRate() : !risk->allowCloseRate()) return CTP_RATE_LIMITED;\n`;
    rc += `      return api->ReqOrderInsert(&f, requestId);\n`;
    rc += `    }\n`;
  } else if (classification === "unsupported-risk-mutation" || classification === "funds") {
    // Requests these stateless rules cannot judge (and funds transfers, which
    // change collateral) are forwarded only with no local rule enabled.
    rc += `    if (risk) {\n`;
    rc += `      bool sendAllowed = false;\n`;
    rc += `      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);\n`;
    rc += `      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;\n`;
    rc += `      return api->${r.name}(&f, requestId);\n`;
    rc += `    }\n`;
  } else if (classification === "conditional-action") {
    rc += `    if (risk && f.ActionFlag != '0') {\n`;
    rc += `      bool sendAllowed = false;\n`;
    rc += `      auto sendGuard = risk->acquireOrderSend(true, sendAllowed);\n`;
    rc += `      if (!sendAllowed || !risk->allowUnsupportedRiskRequest()) return CTP_UNSUPPORTED_RISK_PATH;\n`;
    rc += `      return api->${r.name}(&f, requestId);\n`;
    rc += `    }\n`;
  }
  if (SESSION_TRANSITION_REQUESTS.has(r.name)) {
    rc += `    int sendRc = api->${r.name}(&f, requestId);\n`;
    rc += `    if (sendRc != 0 && risk) risk->finishSessionTransition(requestId);\n`;
    rc += `    return sendRc;\n  }\n`;
  } else if (r.name === "ReqQryInstrument") {
    // A refused send can never be answered: close its staging at once.
    rc += `    int sendRc = api->${r.name}(&f, requestId);\n`;
    rc += `    if (sendRc != 0 && risk) risk->finishInstrumentQuery(requestId, false);\n`;
    rc += `    return sendRc;\n  }\n`;
  } else {
    rc += `    return api->${r.name}(&f, requestId);\n  }\n`;
  }
}
rc += `  default:
    return -1;
  }
}

} // namespace ctp
`;

// Every response-bearing request is settled only by its own response callback
// (or the generic OnRspError). CTP answers each login variant with
// OnRspUserLogin; every other ReqXxx is answered by OnRspXxx. A request without
// a resolvable callback fails generation instead of never settling.
const RESPONSE_CALLBACK_OVERRIDES = new Map([
  ["ReqUserLoginWithCaptcha", "OnRspUserLogin"],
  ["ReqUserLoginWithText", "OnRspUserLogin"],
  ["ReqUserLoginWithOTP", "OnRspUserLogin"],
]);
function responseEvent(r) {
  const callback = RESPONSE_CALLBACK_OVERRIDES.get(r.name) ?? `OnRsp${r.name.slice(3)}`;
  const index = spi.findIndex((s) => s.name === callback);
  if (index < 0 || !spi[index].hasReqId || !spi[index].hasIsLast)
    throw new Error(`no request-correlated response callback ${callback} for ${r.name}`);
  return `${ET_BASE} + ${index + 1}`;
}
// Non-query requests resolve with their first response row, except account
// confirmations, whose every echoed row must be checked by the caller.
const ROW_LIST_REQUESTS = new Set(["ReqSettlementInfoConfirm"]);

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
    const rows = multi || ROW_LIST_REQUESTS.has(r.name);
    const sessionTransition = SESSION_TRANSITION_REQUESTS.has(r.name);
    const evt = responseEvent(r);
    ts += `  ${method}(req: Partial<S.${r.struct}> = {}): Promise<${rows ? "unknown[]" : "unknown"}> {\n`;
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
        ts += `    return this.queryRequest((id) => this.native._req(${i + 1}, encoded, id), ${evt}, ${multi ? "false" : "true"});\n`;
      } else if (r.name !== "ReqUserLogout") {
        ts += `    return this.request((id) => this.native._req(${i + 1}, encoded, id), ${evt}, true, false, true).then((value) => {\n`;
        ts += `      const row = value as { brokerId?: unknown; userId?: unknown; frontId?: unknown; sessionId?: unknown } | undefined;\n`;
        ts += `      if (!row || typeof row.brokerId !== "string" || !row.brokerId || typeof row.userId !== "string" || !row.userId || !Number.isInteger(row.frontId) || Number(row.frontId) <= 0 || !Number.isInteger(row.sessionId) || Number(row.sessionId) === 0 || (typeof cleanReq.brokerId === "string" && cleanReq.brokerId && row.brokerId !== cleanReq.brokerId) || (typeof cleanReq.userId === "string" && cleanReq.userId && row.userId !== cleanReq.userId)) throw new Error("trader login response has missing or mismatched session identity");\n`;
        ts += `      return value;\n    });\n`;
      } else {
        ts += `    return this.request((id) => this.native._req(${i + 1}, encoded, id), ${evt}, true, false, true);\n`;
      }
    } else {
      ts += `    return this.request((id) => this.native._req(${i + 1}, ${enc}, id), ${evt}, ${rows ? "false" : "true"});\n`;
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
