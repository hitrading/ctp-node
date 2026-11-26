// Trader test WITHOUT network. Exercises the full request path end-to-end:
// JS encode (plain camelCase obj -> struct bytes) -> native _req -> generated
// traderReq switch -> CThostFtdcInputOrderField -> pre-trade RiskEngine.
// The risk verdict (which reads the decoded struct fields) proves encode +
// dispatch + risk wiring all agree, with no live front.
import { Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-td", { recursive: true });
const td = new Trader("./flow-td/", "tcp://127.0.0.1:1");
// This offline harness has no real login callback. Seed the JS identity via the
// same post-correlation hook CtpClient uses, then establish the confirmed NATIVE
// session with the gated test hook, which drives the same beginSessionTransition
// -> acceptLoginSession -> confirmSessionAccount handshake the live SPI login +
// session() would. The native gate keeps no position/order ledger, so nothing
// else has to be synchronized before an order reaches the vendor API.
td.onRequestResolved("rsp-user-login", {
  brokerId: "9999", userId: "test", frontId: 1, sessionId: 1, maxOrderRef: "0",
}, {});
process.env.CTP_NODE_TEST_HOOKS = "1";
td.native._injectTestLogin(1, 1, "9999", "test");
console.log("Trader api version:", td.apiVersion);

let pass = 0;
let fail = 0;
const check = (cond, msg) => {
  if (cond) {
    pass++;
    console.log("  PASS:", msg);
  } else {
    fail++;
    console.error("  FAIL:", msg);
  }
};

td.setRiskLimits({ maxOrderVolume: 5 });

// 1) Oversized order -> risk reads VolumeTotalOriginal (100 > 5) and blocks.
//    The rejection message must carry the specific reason (observability).
try {
  await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 100 });
  check(false, "oversized order should have been rejected");
} catch (e) {
  check(/risk/i.test(e.message) && /maxOrderVolume/i.test(e.message), `oversized order blocked, reason surfaced ("${e.message}")`);
}

// 2) Kill-switch -> any order blocked, with the kill-switch reason surfaced.
td.reduceOnly();
try {
  await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 1 });
  check(false, "halted order should have been rejected");
} catch (e) {
  check(/risk/i.test(e.message) && /halt|kill-switch|reduce-only/i.test(e.message), `kill-switch blocked order, reason surfaced ("${e.message}")`);
}
try {
  await td.reqOrderInsert({ instrumentId: "rb2510", direction: "1", combOffsetFlag: "1", limitPrice: 3500, volumeTotalOriginal: 1 });
  check(true, "risk-reducing close stayed available during halt");
} catch (e) {
  check(!/pre-trade risk|kill-switch/i.test(e.message),
    `risk-reducing close stayed available during halt (API outcome: "${e.message}")`);
}
td.resume();

// Complex/parked inserts cannot be judged by the ordinary per-order rules. With
// any local risk configured they must fail closed rather than bypass it.
try {
  await td.reqParkedOrderInsert({});
  check(false, "parked insert should fail closed while risk is configured");
} catch (e) {
  check(/cannot be fully covered/i.test(e.message),
    `parked insert failed closed instead of bypassing risk ("${e.message}")`);
}

// Newer CTP mutations do not end in "Insert". They still change hedge/offset
// state and cannot bypass configured local controls merely because of naming.
for (const [name, send] of [
  ["offset setting", () => td.reqOffsetSetting({})],
  ["spread confirmation", () => td.reqSpdApply({})],
  ["hedge confirmation", () => td.reqHedgeCfm({})],
]) {
  try {
    await send();
    check(false, `${name} should fail closed while risk is configured`);
  } catch (e) {
    check(/cannot be fully covered/i.test(e.message),
      `${name} failed closed instead of bypassing risk ("${e.message}")`);
  }
}

// 3) The gate is stateless: an opening order inside every configured rule passes
//    the native checks with no position/order snapshot and reaches the vendor
//    API. Offline that call fails with the vendor's own send code, never with a
//    native risk sentinel (-10001..-10009).
const r = td
  .reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 1 })
  .then(() => ({ outcome: "resolved" }))
  .catch((e) => ({ outcome: `rejected: ${e.message}`, code: e.ctpReturnCode }));
const sent = await Promise.race([r, new Promise((res) => setTimeout(() => res({ outcome: "pending" }), 250))]);
check(sent.outcome !== "pending" && !(sent.code <= -10001 && sent.code >= -10009) &&
  !/pre-trade risk|metadata unavailable/i.test(sent.outcome),
  `in-limit opening passed the native gate without any ledger sync (outcome: ${sent.outcome})`);

console.log(`TRADER TEST: ${pass} pass, ${fail} fail`);
td.close();
process.exitCode = fail ? 1 : 0;
