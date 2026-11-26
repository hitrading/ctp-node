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

// Each outcome below is matched exactly: { code, message }. With no front
// connected the vendor API refuses every send with -1, so OFFLINE_VENDOR proves
// the request passed every native gate.
const outcomeOf = (promise) => promise.then(
  () => ({ code: 0, message: "sent" }),
  (e) => ({ code: e.ctpReturnCode, message: e.message }));
const same = (actual, expected) => actual.code === expected.code && actual.message === expected.message;
const OFFLINE_VENDOR = { code: -1, message: "order rejected by CTP API (code -1)" };
const RATE_LIMITED = { code: -10002, message: "rate limited (order rate exceeded)" };
const describe = (...outcomes) => outcomes.map((o) => `${o.code} ${o.message}`).join(" | ");

// 4) A real Trader requires a confirmed session from construction. Before any
//    login, with no local rule configured, a request the per-order rules cannot
//    judge is refused and the reason names the missing session; once the
//    account is confirmed the same request reaches the vendor API.
{
  mkdirSync("flow-td-fresh", { recursive: true });
  const fresh = new Trader("./flow-td-fresh/", "tcp://127.0.0.1:1");
  const before = await outcomeOf(fresh.reqCombActionInsert({}));
  check(same(before, {
    code: -10006,
    message: "request is unavailable in the current risk mode or cannot be fully covered by configured local risk controls: session account not confirmed",
  }), `before the first login an unsupported request is refused for the unconfirmed session (${describe(before)})`);
  fresh.native._injectTestLogin(1, 1, "9999", "test");
  const after = await outcomeOf(fresh.reqCombActionInsert({}));
  check(same(after, OFFLINE_VENDOR),
    `after the account is confirmed the same request passes the native gate (${describe(after)})`);
  fresh.close();
}

// 4b) The settlement confirmation must echo the confirmed account. A mismatched
//     echo, a refused confirmation or a failed step after it fails the handshake
//     and revokes both the JS identity and the native confirmed account.
{
  mkdirSync("flow-td-settle", { recursive: true });
  const settle = new Trader("./flow-td-settle/", "tcp://127.0.0.1:1");
  settle.reqUserLogin = async () => {
    settle.native._injectTestLogin(1, 7, "9999", "u");
    return { brokerId: "9999", userId: "u", frontId: 1, sessionId: 7 };
  };
  const session = (sync = { multipliers: false }) =>
    settle.session({ brokerId: "9999", userId: "u", password: "p", sync }).then(() => null, (e) => e);
  const UNCONFIRMED = {
    code: -10006,
    message: "request is unavailable in the current risk mode or cannot be fully covered by configured local risk controls: session account not confirmed",
  };
  const expectRevoked = async (label) => {
    const outcome = await outcomeOf(settle.reqCombActionInsert({}));
    check(settle.broker === undefined && settle.investor === undefined && same(outcome, UNCONFIRMED),
      `${label} leaves no JS or native session identity (${describe(outcome)})`);
  };
  for (const [label, confirm, expected] of [
    ["an echo naming another broker", async () => ({ brokerId: "8888", investorId: "u" }),
      "settlement confirmation names a different broker or investor"],
    ["an echo naming another investor", async () => ({ brokerId: "9999", investorId: "other" }),
      "settlement confirmation names a different broker or investor"],
    ["a refused confirmation", async () => { throw new Error("CTP refused the settlement confirmation"); },
      "CTP refused the settlement confirmation"],
  ]) {
    settle.reqSettlementInfoConfirm = confirm;
    const error = await session();
    check(error?.message === expected, `${label} fails the handshake (${error?.message})`);
    await expectRevoked(label);
  }
  for (const [label, echo] of [["a matching echo", { brokerId: "9999", investorId: "u" }],
    ["an empty echo", undefined], ["an echo without account fields", {}]]) {
    settle.reqSettlementInfoConfirm = async () => echo;
    const error = await session();
    check(error === null && settle.broker === "9999" && settle.investor === "u",
      `${label} completes the handshake (${error?.message ?? "ok"})`);
  }
  settle.reqSettlementInfoConfirm = async () => ({});
  settle.reqQryInstrument = async () => { throw new Error("instrument catalog unavailable"); };
  const syncError = await session({ multipliers: true });
  check(syncError?.message === "instrument catalog unavailable",
    `a failed metadata sync after confirmation fails the handshake (${syncError?.message})`);
  await expectRevoked("a failed metadata sync after confirmation");
  settle.close();
}

// 5) Opening and closing inserts draw from separate token buckets, and HALT_ALL
//    refuses closes too. A rate of one token per 1000 s cannot refill mid-test.
{
  mkdirSync("flow-td-gates", { recursive: true });
  const gates = new Trader("./flow-td-gates/", "tcp://127.0.0.1:1");
  gates.onRequestResolved("rsp-user-login", {
    brokerId: "9999", userId: "test", frontId: 1, sessionId: 1, maxOrderRef: "0",
  }, {});
  gates.native._injectTestLogin(1, 1, "9999", "test");
  const insert = (direction, combOffsetFlag) => outcomeOf(gates.reqOrderInsert({
    instrumentId: "rb2510", direction, combOffsetFlag, limitPrice: 3500, volumeTotalOriginal: 1,
  }));
  const open = () => insert("0", "0");
  const close = () => insert("1", "1");
  const oneTokenEach = { maxOrdersPerSec: 0.001, orderBurst: 1, maxCloseOrdersPerSec: 0.001, closeOrderBurst: 1 };

  gates.setRiskLimits(oneTokenEach);
  const firstOpen = await open(), drainedOpen = await open(), closeAfterOpens = await close();
  check(same(firstOpen, OFFLINE_VENDOR) && same(drainedOpen, RATE_LIMITED) && same(closeAfterOpens, OFFLINE_VENDOR),
    `an exhausted opening bucket returns -10002 while a close still passes (${describe(firstOpen, drainedOpen, closeAfterOpens)})`);

  gates.setRiskLimits(oneTokenEach); // refill both buckets
  const firstClose = await close(), drainedClose = await close(), openAfterCloses = await open();
  check(same(firstClose, OFFLINE_VENDOR) && same(drainedClose, RATE_LIMITED) && same(openAfterCloses, OFFLINE_VENDOR),
    `an exhausted closing bucket returns -10002 while an open still passes (${describe(firstClose, drainedClose, openAfterCloses)})`);

  gates.setRiskLimits({});
  gates.haltAll();
  const haltedClose = await close();
  check(same(haltedClose, { code: -10001, message: "blocked by pre-trade risk: all order inserts halted" }),
    `HALT_ALL refuses a closing insert as halted (${describe(haltedClose)})`);
  gates.resume();
  gates.close();
}

console.log(`TRADER TEST: ${pass} pass, ${fail} fail`);
td.close();
process.exitCode = fail ? 1 : 0;
