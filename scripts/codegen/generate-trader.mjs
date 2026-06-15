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
const HEADER = new URL("tradeapi/ThostFtdcTraderApi.h", ROOT);
const OUT = new URL("src/generated/", ROOT);

const { spi: spiAll, req: reqAll } = await parseTraderApi(HEADER);

// Dedupe by name (defensive).
const seen = new Set();
const spi = spiAll.filter((s) => (seen.has(s.name) ? false : (seen.add(s.name), true)));
const seenR = new Set();
const req = reqAll.filter((r) => (seenR.has(r.name) ? false : (seenR.add(r.name), true)));

const kebab = (method) => splitWords(method.replace(/^On/, "")).map((w) => w.toLowerCase()).join("-");
const member = (method) => method.replace(/^On/, "");

const ET_BASE = 0x2000;

// ---------- traderspi.gen.h ----------
let h = `/* AUTO-GENERATED. Do not edit. */
#ifndef CTP_TRADERSPI_GEN_H
#define CTP_TRADERSPI_GEN_H
#include "ThostFtdcTraderApi.h"
#include "../native/channel.h"
namespace ctp {
enum {
  ET_BASE = ${ET_BASE},
`;
spi.forEach((s, i) => (h += `  ET_${member(s.name)} = ET_BASE + ${i + 1},\n`));
h += `};

class TraderSpi : public CThostFtdcTraderSpi {
public:
  TraderSpi(CThostFtdcTraderApi *api, EventChannel *channel) : api_(api), ch_(channel) {}
  ~TraderSpi() {}

`;
for (const s of spi) h += `  void ${s.name}(${rawDecl(s)}) override;\n`;
h += `
private:
  CThostFtdcTraderApi *api_;
  EventChannel *ch_;
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
#include "../native/risk.h"
#include <algorithm>
#include <cstring>

namespace ctp {

int traderReq(CThostFtdcTraderApi *api, int methodId, const void *bytes, int len,
              int requestId, RiskEngine *risk) {
  switch (methodId) {
`;
for (const r of req) {
  const T = `CThostFtdc${r.struct}Field`;
  rc += `  case M_${r.name}: {\n`;
  rc += `    ${T} f;\n    std::memset(&f, 0, sizeof(f));\n`;
  rc += `    std::memcpy(&f, bytes, std::min(static_cast<size_t>(len), sizeof(f)));\n`;
  if (r.name === "ReqOrderInsert") {
    rc += `    if (risk) {\n`;
    rc += `      RiskVerdict v = risk->check(f.LimitPrice, 0.0, f.VolumeTotalOriginal);\n`;
    rc += `      if (!v.ok) return CTP_RISK_BLOCKED;\n`;
    rc += `      if (!risk->allowRate()) return CTP_RATE_LIMITED;\n`;
    rc += `    }\n`;
  }
  rc += `    return api->${r.name}(&f, requestId);\n  }\n`;
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
req.forEach((r, i) => {
  const method = camelCase(r.name);
  const multi = r.name.startsWith("ReqQry");
  const ret = multi ? "unknown[]" : "unknown";
  ts += `  ${method}(req: Partial<S.${r.struct}> = {}): Promise<${ret}> {\n`;
  ts += `    return this.request((id) => this.native._req(${i + 1}, this.encode(STRUCT_ID.${r.struct}, req as Record<string, unknown>), id), ${!multi});\n`;
  ts += `  }\n`;
});
ts += `}\n`;

await writeFile(new URL("traderspi.gen.h", OUT), h);
await writeFile(new URL("traderspi.gen.cc", OUT), c);
await writeFile(new URL("traderreq.gen.h", OUT), rh);
await writeFile(new URL("traderreq.gen.cc", OUT), rc);
await writeFile(new URL("trader.gen.ts", OUT), ts);

console.log(`generated trader: ${spi.length} SPI callbacks, ${req.length} req methods -> src/generated/`);
