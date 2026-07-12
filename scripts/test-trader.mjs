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
// session() would. Position/order LEDGERS deliberately stay unsynchronized, so
// the opening fail-closed case below is still real.
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

// Complex/parked inserts cannot use the normal order reservation ledger. With
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

// 3) A newly created Trader has no authoritative position/order snapshots yet.
//    Even with scalar checks satisfied, opening must remain fail-closed.
const r = td
  .reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 1 })
    .then(() => "resolved")
  .catch((e) => `rejected: ${e.message}`);
const outcome = await Promise.race([r, new Promise((res) => setTimeout(() => res("pending"), 250))]);
check(/metadata unavailable/i.test(String(outcome)), `unsynchronized opening stayed fail-closed (outcome: ${outcome})`);

console.log(`TRADER TEST: ${pass} pass, ${fail} fail`);
td.close();
process.exitCode = fail ? 1 : 0;
