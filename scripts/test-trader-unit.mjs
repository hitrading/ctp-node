// Coverage for trader.ts logic the e2e tests don't reach offline: the stateless
// risk setters + finite guards, order validation, the rsp-user-login
// credential/session listener, and session()/sync* (driven by stubbing the leaf
// req query methods so the row-processing logic runs without a live
// connection). The native risk-engine calls underneath run for real.
import { Trader, MarketData } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-tu", { recursive: true });
mkdirSync("flow-tu-md", { recursive: true });
mkdirSync("flow-tu-signed-login", { recursive: true });
mkdirSync("flow-tu-signed-session", { recursive: true });
mkdirSync("flow-tu-zero-session", { recursive: true });
mkdirSync("flow-tu-paced", { recursive: true });
let pass = 0, fail = 0;
const ok = (c, m) => { c ? (pass++, console.log("  PASS:", m)) : (fail++, console.log("  FAIL:", m)); };
const throws = (fn, m) => { try { fn(); ok(false, m + " (did not throw)"); } catch { ok(true, m); } };
const rejects = async (p, m) => { try { await p; ok(false, m + " (did not reject)"); } catch { ok(true, m); } };
// With no front connected the vendor API refuses every send with -1, so exactly
// this rejection proves the request passed every native gate. Fire-and-forget
// inserts/actions report "order", response-bearing requests "request".
const refusedByOfflineVendor = (e, kind) =>
  e?.ctpReturnCode === -1 && e.message === `${kind} rejected by CTP API (code -1)`;

const td = new Trader("./flow-tu/", "tcp://127.0.0.1:1");
ok(td.queryRetryDelayMs === 1100, "sync helpers pause a full second between attempts by default");
td.queryRetryDelayMs = 0; // the pause itself is covered separately
delete process.env.CTP_NODE_TEST_HOOKS;
throws(() => td.native._burstEvents(1), "queue-producing Trader test hooks are disabled by default");
process.env.CTP_NODE_TEST_HOOKS = "1";
throws(() => new Trader("./flow\0evil/", "tcp://127.0.0.1:1"),
  "Trader rejects an embedded NUL flowPath before entering CTP");
for (const removed of [
  "getApiVersion", "getTradingDay", "riskSet", "halt", "positionCost",
  "positionMargin", "setMaxPosition", "setMaxPositions",
  "setMaxInstrumentPosition", "setMaxInstrumentPositions",
  "setMaxInstrumentMargin", "setMaxInstrumentMargins",
  "seedPosition",
  // 5.0.0: the native position/order ledger and armed orders are gone.
  "setInstrumentPositionLimit", "setInstrumentPositionLimits",
  "setInstrumentMarginLimit", "setInstrumentMarginLimits",
  "syncPositions", "syncOrders", "seedPositions", "seedFromPositions",
  "resetPositions", "accountMargin", "arm", "armStats", "_armRegistry",
  "_armFireCount", "_armBlockedCount",
]) {
  ok(typeof td[removed] === "undefined", `${removed} compatibility API is absent`);
}
for (const removed of [
  "lastMarginRejection", "_setInstrumentPositionLimits", "_setInstrumentMarginLimits",
  "rebuildReservations", "riskRevision", "_markPositionLedgerUncertain",
  "_markOrderLedgerUncertain", "replacePositions", "accountMargin", "resetPositions",
  "_armRegistry", "arm", "disarm", "_armFireCount", "_armBlockedCount", "_seedPositions",
]) {
  ok(typeof td.native[removed] === "undefined", `native ${removed} is absent`);
}

// Only a response correlated to a currently pending request may update the
// confirmed JS identity. Public stale protocol events remain observable but
// have no identity side effect.
const resolveSession = (event, value) => td.onRequestResolved(event, value, {});
resolveSession("rsp-user-login", { brokerId: "9999", userId: "u", maxOrderRef: "100", frontId: 1, sessionId: 7 });
resolveSession("rsp-unrelated", { brokerId: "bad", userId: "bad" });
td.emit("rsp-user-login", { brokerId: "bad", userId: "bad", maxOrderRef: "999" },
  { rspInfo: { errorId: 7, errorMsg: "rejected" } });
resolveSession("rsp-user-login", {});
resolveSession("rsp-user-login", undefined);
resolveSession("rsp-user-login", { maxOrderRef: "  101  " });
ok(td.broker === "9999" && td.investor === "u" && td.orderRefSeq === 101,
  "matched login caches partial/empty payloads and trims padded maxOrderRef while stale events are ignored");
td.emit("rsp-user-logout", undefined, { rspInfo: { errorId: 7, errorMsg: "rejected" } });
ok(td.broker === "9999" && td.investor === "u",
  "an unmatched/rejected logout event preserves the confirmed session identity");
resolveSession("rsp-user-logout", undefined);
ok(td.broker === undefined && td.investor === undefined,
  "a matched successful logout revokes the confirmed session identity");
// A login row lacking a usable FrontID/SessionID is rejected by the generated
// login validator; onRequestResolved must NOT cache broker/investor from it,
// otherwise a rejected login would strand a stale confirmed identity.
resolveSession("rsp-user-login", { brokerId: "7777", userId: "z", maxOrderRef: "5", frontId: 0, sessionId: 7 });
ok(td.broker === undefined && td.investor === undefined,
  "a login row with FrontID 0 does not cache a confirmed identity (matches the login validator)");
resolveSession("rsp-user-login", { brokerId: "7777", userId: "z", frontId: 1, sessionId: 0 });
ok(td.broker === undefined && td.investor === undefined,
  "a login row with SessionID 0 does not cache a confirmed identity");
resolveSession("rsp-user-login", {
  brokerId: "7777", userId: "z", maxOrderRef: "6", frontId: 1, sessionId: -2147483647,
});
ok(td.broker === "7777" && td.investor === "z",
  "a non-zero negative signed SessionID caches the confirmed identity");
resolveSession("rsp-user-logout", undefined);
resolveSession("rsp-user-login", { brokerId: "7777", userId: "z" });
ok(td.broker === undefined && td.investor === undefined,
  "a login row without FrontID/SessionID at all does not cache a confirmed identity");
resolveSession("rsp-user-login", { brokerId: "9999", userId: "u", maxOrderRef: "101", frontId: 1, sessionId: 7 });
await rejects(td.reqUserLogout({ brokerId: 1 }),
  "a malformed logout is rejected before changing the confirmed identity");
ok(td.broker === "9999" && td.investor === "u",
  "a preflight-rejected session transition preserves confirmed identity");
// Stub only the native send for this JS identity test; invoking the offline C++
// logout path would deliberately invalidate the native risk session used by
// later tests, while no real login callback exists here to establish a new one.
const nativeForIdentityTransition = td.native;
td.native = new Proxy(nativeForIdentityTransition, {
  get(target, prop) {
    if (prop === "_req") return () => 0;
    const value = Reflect.get(target, prop, target);
    return typeof value === "function" ? value.bind(target) : value;
  },
});
const identityRevokingLogout = td.reqUserLogout({ brokerId: "9999", userId: "u" });
td.native = nativeForIdentityTransition;
identityRevokingLogout.catch(() => {});
ok(td.broker === undefined && td.investor === undefined,
  "a valid login/logout transition revokes confirmed identity immediately");
resolveSession("rsp-user-login", { brokerId: "9999", userId: "u", maxOrderRef: "101", frontId: 1, sessionId: 7 });

// The generated login validator must preserve CTP's signed 32-bit SessionID.
// SimNow can return a negative non-zero value; only zero means no session.
const signedLoginTrader = new Trader("./flow-tu-signed-login/", "tcp://127.0.0.1:1");
const signedLoginNative = signedLoginTrader.native;
let generatedSessionId = -2147483647;
signedLoginTrader.native = new Proxy(signedLoginNative, {
  get(target, prop) {
    if (prop === "_req") return (_opcode, _encoded, requestId) => {
      queueMicrotask(() => signedLoginTrader.deliver({
        evt: 8192 + 6,
        value: {
          brokerId: "9999", userId: "signed", frontId: 1,
          sessionId: generatedSessionId, maxOrderRef: "0",
        },
        options: { requestId, isLast: true },
      }));
      return 0;
    };
    const value = Reflect.get(target, prop, target);
    return typeof value === "function" ? value.bind(target) : value;
  },
});
const generatedNegativeLogin = await signedLoginTrader.reqUserLogin({
  brokerId: "9999", userId: "signed", password: "p",
});
ok(generatedNegativeLogin.sessionId === -2147483647,
  "generated reqUserLogin accepts and preserves a negative non-zero SessionID");
generatedSessionId = 0;
await rejects(signedLoginTrader.reqUserLogin({
  brokerId: "9999", userId: "signed", password: "p",
}), "generated reqUserLogin still rejects SessionID 0");
signedLoginTrader.native = signedLoginNative;
signedLoginTrader.close();

// Query spacing follows the limits the logged-in front grants, and falls back to
// the default on every login/logout transition and front change.
{
  const paced = new Trader("./flow-tu-paced/", "tcp://127.0.0.1:1");
  const pacedNative = paced.native;
  let limits = { frontAddr: "tcp://front", qryFreq: 10, ftdPkgFreq: 10 };
  paced.native = new Proxy(pacedNative, {
    get(target, prop) {
      if (prop === "frontInfo") return () => limits;
      if (prop === "_req") return () => 0;
      const value = Reflect.get(target, prop, target);
      return typeof value === "function" ? value.bind(target) : value;
    },
  });
  const login = (row) => paced.onRequestResolved("rsp-user-login",
    { brokerId: "9999", userId: "u", maxOrderRef: "1", frontId: 1, sessionId: 7, ...row }, {});
  ok(paced.queryIntervalMs === 1100, "queries start at the default spacing");
  login({ frontId: 0 });
  ok(paced.queryIntervalMs === 1100, "a login without a usable session keeps the default spacing");
  login({});
  ok(paced.frontInfo.frontAddr === "tcp://front" && paced.queryIntervalMs === 220,
    "a confirmed login paces queries at half of the front's packet budget");
  paced.emit("front-disconnected", 4097, {});
  ok(paced.queryIntervalMs === 1100, "a front change restores the default spacing");
  limits = { ...limits, qryFreq: 2147483647, ftdPkgFreq: 10000 };
  login({});
  ok(paced.queryIntervalMs === 110, "an unlimited front is still capped");
  paced.reqUserLogout({ brokerId: "9999", userId: "u" }).catch(() => {});
  ok(paced.queryIntervalMs === 1100, "a logout transition restores the default spacing");
  paced.native = pacedNative;
  paced.close();
  const closed = paced.frontInfo;
  ok(closed.frontAddr === "" && closed.qryFreq === 0 && closed.ftdPkgFreq === 0, "frontInfo is empty after close");
}

// Generated queries snapshot their request before entering the paced queue; a
// later accessor result or object mutation cannot change the wire payload.
td.lastQuerySentAt = performance.now();
let queryAccessorReads = 0;
const delayedQueryRequest = {};
Object.defineProperty(delayedQueryRequest, "instrumentId", {
  enumerable: true,
  get() { queryAccessorReads++; return queryAccessorReads === 1 ? "rb2510" : "mutated"; },
});
const snapshottedQuery = td.reqQryInstrument(delayedQueryRequest);
ok(queryAccessorReads === 1, "generated query parameters are encoded at invocation");
td.emit("front-disconnected", 1, {});
await rejects(snapshottedQuery, "queued query is revoked on a front transition");
resolveSession("rsp-user-login", { brokerId: "9999", userId: "u", maxOrderRef: "101", frontId: 1, sessionId: 7 });

// risk config + finite guards.
td.setRiskLimits({ maxOrderVolume: 10, maxNotional: 1e6, maxPriceDeviation: 0.02, maxOrdersPerSec: 5, orderBurst: 10, maxCloseOrdersPerSec: 2, closeOrderBurst: 2, maxReferenceAgeMs: 5000 });
ok(true, "setRiskLimits applies a full config");
throws(() => td.setRiskLimits({ maxAccountMargin: 1e7 }),
  "setRiskLimits rejects the removed maxAccountMargin field instead of silently ignoring it");
throws(() => td.native.setRiskLimits({ maxAccountMargin: 1e7 }),
  "native setRiskLimits rejects the removed maxAccountMargin field");
throws(() => td.setRiskLimits({ maxOrderVolume: Infinity }), "setRiskLimits rejects non-finite maxOrderVolume");
throws(() => td.setRiskLimits({ maxOrderVolume: 1.5 }), "setRiskLimits rejects a fractional order-volume cap");
throws(() => td.setRiskLimits({ maxNotional: NaN }), "setRiskLimits rejects NaN maxNotional");
throws(() => td.setRiskLimits({ maxNotional: -1 }), "setRiskLimits rejects a negative cap instead of disabling it");
throws(() => td.setRiskLimits({ maxMargin: 1e6 }), "setRiskLimits rejects removed/unknown fields instead of silently disabling risk");
throws(() => td.setRiskLimits(null), "setRiskLimits rejects null");
throws(() => td.setRiskLimits(Object.create({ maxOrderVolume: 1 })),
  "setRiskLimits rejects inherited config fields / non-plain objects");
Object.defineProperty(Object.prototype, "maxOrderVolume", {
  value: -1, configurable: true,
});
try {
  td.setRiskLimits({});
  ok(true, "setRiskLimits ignores polluted Object.prototype fields");
} catch {
  ok(false, "setRiskLimits ignores polluted Object.prototype fields");
} finally {
  delete Object.prototype.maxOrderVolume;
}
const nullPrototypeRisk = Object.assign(Object.create(null), { maxOrderVolume: 2 });
td.setRiskLimits(nullPrototypeRisk);
ok(true, "setRiskLimits accepts an explicit null-prototype dictionary");
throws(() => td.setRiskLimits({ [Symbol("hidden")]: 1 }),
  "setRiskLimits rejects symbol config fields");
td.reduceOnly(); td.resume(); td.haltAll(); td.resume();
ok(true, "NORMAL / REDUCE_ONLY / HALT_ALL transitions");
throws(() => td.setRiskLimits({ maxReferenceAgeMs: 0 }), "setRiskLimits rejects a zero reference-age window");
ok(typeof td.apiVersion === "string", "apiVersion property");
ok(typeof td.tradingDay === "string", "tradingDay property");
{
  const front = td.frontInfo;
  ok(typeof front.frontAddr === "string" && Number.isInteger(front.qryFreq) && Number.isInteger(front.ftdPkgFreq),
    "frontInfo exposes the front address and its per-second limits");
}

// reqOrderInsert: auto orderRef when blank, and keep an explicit one (rejects offline; logic runs).
await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500 }).catch(() => {});
await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, orderRef: "103" }).catch(() => {});
ok(true, "reqOrderInsert (auto + explicit orderRef)");
// An oversized instrumentId passes validateOrder (non-empty, no NUL) but fails
// the encode preflight (trader.ts:553) which runs BEFORE commitOrderRef, so the
// explicit OrderRef must not be consumed. (A broker mismatch would throw earlier
// inside validateOrder and never reach the encode path.)
const beforeEncFail = td.orderRefSeq;
throws(() => td.reqOrderInsert({ instrumentId: "x".repeat(100), direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, orderRef: "500" }),
  "an encoding failure is rejected before consuming its explicit OrderRef");
ok(td.orderRefSeq === beforeEncFail, "a failed encode preflight does not consume the explicit OrderRef");
await td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, orderRef: "500" }).catch(() => {});
ok(td.orderRefSeq === 500, "the OrderRef is then consumable by a valid send");
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, orderRef: "bad-ref" }), "reqOrderInsert rejects a non-numeric OrderRef");
for (const [label, fields] of [
  ["a missing instrumentId", { direction: "0" }],
  ["an empty instrumentId", { instrumentId: "", direction: "0" }],
  ["a non-string instrumentId", { instrumentId: 2510, direction: "0" }],
  ["an embedded NUL instrumentId", { instrumentId: "rb\0evil", direction: "0" }],
  ["a missing direction", { instrumentId: "rb" }],
  ["an unknown direction", { instrumentId: "rb", direction: "2" }],
]) {
  let identityError;
  try {
    td.reqOrderInsert({ combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, ...fields });
  } catch (e) { identityError = e; }
  ok(identityError instanceof TypeError &&
     /instrumentId and direction \("0"\/"1"\) are required/.test(identityError.message),
    `reqOrderInsert rejects ${label} before encoding`);
}
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", volumeTotalOriginal: 1 }), "reqOrderInsert rejects a missing offset flag");
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", combHedgeFlag: "", volumeTotalOriginal: 1 }),
  "reqOrderInsert rejects a missing hedge flag override");
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", combHedgeFlag: "4", volumeTotalOriginal: 1 }),
  "reqOrderInsert rejects an invalid hedge flag");
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 0 }),
  "reqOrderInsert rejects zero volume");
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1.5 }),
  "reqOrderInsert rejects fractional volume");
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: -1 }),
  "reqOrderInsert rejects a negative limit price");
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: NaN }),
  "reqOrderInsert rejects a non-finite limit price");
// limitPrice present so validateOrder reaches the broker/investor-match checks
// (it validates limitPrice first; without it these would throw on the wrong path).
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, brokerId: "other" }),
  "reqOrderInsert rejects a broker override outside the confirmed login");
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, investorId: "other" }),
  "reqOrderInsert rejects an investor override outside the confirmed login");
const beforeStructural = td.orderRefSeq;
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", volumeTotalOriginal: 1, orderRef: "999" }),
  "a malformed order is rejected before consuming its explicit OrderRef");
ok(td.orderRefSeq === beforeStructural,
  "the structural rejection leaves the explicit OrderRef 999 unconsumed (high-water mark unchanged)");
await td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, orderRef: "999" }).catch(() => {});
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, orderRef: "102" }),
  "reqOrderInsert rejects an explicit OrderRef at/below the high-water mark");
const savedOrderRefSeq = td.orderRefSeq;
td.orderRefSeq = 999_999_999_999;
throws(() => td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500 }),
  "auto OrderRef allocation fails closed when the session space is exhausted");
td.orderRefSeq = savedOrderRefSeq;
for (const flags of ["00", "30", "03"]) {
  throws(
    () => td.reqOrderInsert({ instrumentId: "mixed", direction: "0", combOffsetFlag: flags, volumeTotalOriginal: 1, limitPrice: 1 }),
    `multi-leg offset ${flags} is rejected before it can bypass local risk`
  );
}
for (const policy of [
  { name: "FAK", timeCondition: "1", volumeCondition: "1" },
  { name: "FOK", timeCondition: "1", volumeCondition: "3" },
]) {
  const result = td.reqOrderInsert({
    instrumentId: "rb", direction: "0", combOffsetFlag: "1",
    volumeTotalOriginal: 1, limitPrice: 3500,
    timeCondition: policy.timeCondition, volumeCondition: policy.volumeCondition,
  });
  ok(result instanceof Promise, `reqOrderInsert accepts a native ${policy.name} limit order`);
  const outcome = await result.then(() => undefined, (error) => error);
  ok(outcome?.ctpReturnCode !== -10008,
    `native ordinary-order capability accepts ${policy.name}`);
}
// Explicit empty strings mean the same as omitted optional text fields; any
// non-empty routing/account override is outside the ordinary-order capability.
const explicitEmpty = td.reqOrderInsert({
  instrumentId: "rb", direction: "0", combOffsetFlag: "1", volumeTotalOriginal: 1, limitPrice: 3500,
  gtdDate: "", businessUnit: "", exchangeId: "", investUnitId: "", accountId: "", currencyId: "", clientId: "",
});
ok(explicitEmpty instanceof Promise, "reqOrderInsert treats explicit empty optional text fields as omitted");
await explicitEmpty.catch(() => {});
for (const field of ["gtdDate", "businessUnit", "exchangeId", "investUnitId", "accountId", "currencyId", "clientId"]) {
  let overrideError;
  try {
    td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "1", volumeTotalOriginal: 1,
      limitPrice: 3500, [field]: "X" });
  } catch (e) { overrideError = e; }
  ok(/only single-instrument GFD, FAK or FOK limit orders are supported/.test(String(overrideError?.message)),
    `reqOrderInsert rejects a non-empty ${field} override`);
}
for (const invalidPolicy of [
  { timeCondition: "5", volumeCondition: "1" },
  { timeCondition: "3", volumeCondition: "3" },
  { timeCondition: "1", volumeCondition: "2" },
]) {
  throws(() => td.reqOrderInsert({
    instrumentId: "rb", direction: "0", combOffsetFlag: "0",
    volumeTotalOriginal: 1, limitPrice: 3500, ...invalidPolicy,
  }), "reqOrderInsert rejects an unsupported time/volume condition combination");
}
ok(td.shouldTimeoutRequest(999_999, false) === true, "a non-session-transition request always times out");
ok(td.shouldTimeoutRequest(999_999, true) === false, "an unmatched session-transition timeout is linearized natively (no pending -> false)");

const md = new MarketData("./flow-tu-md/", "tcp://127.0.0.1:1");

// trackMarketData wires the MD snapshot cache into the risk engine in C++ (the
// deviation reference). Wire the shared td (harmless for later tests) and then
// verify the wiring is REAL end-to-end on isolated instances: with a wired live
// reference an order NEAR the price passes the deviation gate while a FAR one is
// blocked. (A block alone can't prove wiring — a missing reference also blocks —
// so the discriminating signal is an ALLOW near the reference. The C++ harness
// only ever exercises the deviation logic through MockSnap/setMdSnapshot, never
// the real SnapshotCache -> _snapshotExternal() -> _attachSnapshot path.)
td.trackMarketData(md);
{
  mkdirSync("flow-tu-track", { recursive: true });
  mkdirSync("flow-tu-track-md", { recursive: true });
  const mdTrack = new MarketData("./flow-tu-track-md/", "tcp://127.0.0.1:1");
  mdTrack._injectTestLifecycle("login");
  mdTrack._injectTestTick(3499, 3501); // md's real cache: rb2510 lastPrice ~3500.5
  const tdTrack = new Trader("./flow-tu-track/", "tcp://127.0.0.1:1");
  tdTrack.native._injectTestLogin(1, 7, "9999", "u");
  tdTrack.broker = "9999"; tdTrack.investor = "u";
  tdTrack.trackMarketData(mdTrack);
  tdTrack.setRiskLimits({ maxPriceDeviation: 0.01, maxReferenceAgeMs: 60000 });
  const outcome = (limitPrice) => tdTrack.reqOrderInsert({
    instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice,
  }).then(() => "sent", (e) => String(e?.message ?? e));
  const near = await outcome(3500); // ~0.01% off the wired reference -> passes deviation
  const far = await outcome(5000);  // ~43% off -> blocked ONLY if the reference is wired
  ok(!/deviat|reference/i.test(near) && /deviat/i.test(far),
    "trackMarketData wires the MD snapshot: near-reference order passes the deviation gate, a far one is blocked (would fail closed on 'reference' if unwired)");
  // maxNotional reads the same wired reference through the real order path. The
  // multiplier comes only from OnRspQryInstrument inside C++, so before any
  // instrument query an enabled notional cap fails closed with the specific
  // reason as an ordinary pre-trade block (no ledger-era -10007 / evidence).
  tdTrack.setRiskLimits({ maxNotional: 1e9, maxReferenceAgeMs: 60000 });
  const submit = (direction, combOffsetFlag) => tdTrack.reqOrderInsert({ instrumentId: "rb2510", direction,
    combOffsetFlag, combHedgeFlag: "1", limitPrice: 3501, volumeTotalOriginal: 1 }).then(() => null, (error) => error);
  const missing = await submit("0", "0");
  ok(missing?.ctpReturnCode === -10001 && /instrument multiplier unavailable/.test(missing.message) &&
    !("marginRejection" in missing),
    "maxNotional without a synchronized multiplier fails closed, naming the missing multiplier");
  const closing = await submit("1", "1");
  ok(refusedByOfflineVendor(closing, "order"),
    `a risk-reducing close passes every native gate while the multiplier is missing (${closing?.message ?? "sent"})`);
  tdTrack.close(); mdTrack.close();
}

const signedSessionTrader = new Trader("./flow-tu-signed-session/", "tcp://127.0.0.1:1");
signedSessionTrader.reqUserLogin = async () => {
  const sessionId = -2147483647;
  ok(signedSessionTrader.native._injectTestLogin(1, sessionId, "9999", "signed"),
    "native login/account confirmation accepts a negative non-zero SessionID");
  return {
    brokerId: "9999", userId: "signed", maxOrderRef: "42", frontId: 1, sessionId,
  };
};
for (const [label, investorId] of [["an empty", ""], ["a non-string", 7], ["a NUL-bearing", "inv\0x"]]) {
  let investorError;
  await signedSessionTrader.session({ brokerId: "9999", userId: "signed", password: "p", investorId })
    .catch((e) => { investorError = e; });
  ok(investorError instanceof TypeError &&
     investorError.message === "session investorId must be a non-empty string",
    `session rejects ${label} explicit investorId before changing context`);
}
const signedSession = await signedSessionTrader.session({
  brokerId: "9999", userId: "signed", password: "p", confirmSettlement: false,
  sync: { multipliers: false },
});
ok(signedSession.identity.sessionId === -2147483647,
  "session() preserves the exact negative signed SessionID");
ok(signedSession.identity.maxOrderRef === 42 && signedSessionTrader.orderRefSeq === 42,
  "session() raises the automatic OrderRef high-water mark to the login's higher MaxOrderRef");
signedSessionTrader.close();

const zeroSessionTrader = new Trader("./flow-tu-zero-session/", "tcp://127.0.0.1:1");
ok(!zeroSessionTrader.native._injectTestLogin(1, 0, "9999", "zero"),
  "native login/account confirmation still rejects SessionID 0");
zeroSessionTrader.close();

// session() + sync* with the leaf req methods stubbed (no network).
td.reqAuthenticate = async () => ({});
td.reqUserLogin = async () => {
  // Offline: drive the native login handshake the live SPI would, so the
  // session()'s _confirmSessionAccount can reach a confirmed native session.
  td.native._injectTestLogin(1, 7, "9999", "u");
  return { brokerId: "9999", userId: "u", maxOrderRef: "100", frontId: 1, sessionId: 7 };
};
td.reqSettlementInfoConfirm = async () => ({});
td.reqQryInstrument = async () => [{ instrumentId: "rb2510", productId: "rb", volumeMultiple: 10 }, { instrumentId: "x", volumeMultiple: 0 }];
let exchangeMarginQueries = 0;
td.reqQryExchangeMarginRate = async (request) => { exchangeMarginQueries++; return [{
  instrumentId: "rb2510", hedgeFlag: request.hedgeFlag,
  longMarginRatioByMoney: 0.08, longMarginRatioByVolume: 0,
  shortMarginRatioByMoney: 0.08, shortMarginRatioByVolume: 0,
}]; };
const sessionMarginHedgeFlags = [];
td.reqQryInstrumentMarginRate = async (request) => {
  sessionMarginHedgeFlags.push(request.hedgeFlag);
  return [{
  instrumentId: "rb2510", hedgeFlag: request.hedgeFlag,
  isRelative: 0,
  longMarginRatioByMoney: 0.1, longMarginRatioByVolume: 0,
  shortMarginRatioByMoney: 0.1, shortMarginRatioByVolume: 0,
}];
};
// session() no longer snapshots positions or working orders: the native gate
// keeps no ledger, so the handshake must not issue either query.
let ledgerQueries = 0;
td.reqQryInvestorPosition = async () => { ledgerQueries++; return []; };
td.reqQryOrder = async () => { ledgerQueries++; return []; };
const res = await td.session({ brokerId: "9999", userId: "u", password: "p", appId: "a", authCode: "c" });
ok(res.multipliers === 1, "session: syncMultipliers applied 1 (skipped volumeMultiple 0)");
ok(Object.keys(res).sort().join() === "identity,marginRates,multipliers" && ledgerQueries === 0,
  "session: the result carries no positions/orders counts and the handshake queries no ledger");
const res2 = await td.session({ brokerId: "9999", userId: "u", password: "p", confirmSettlement: false, sync: { multipliers: ["rb2510"] } });
ok(res2.multipliers === 1 && res2.marginRates === 0, "session: auth skipped + settlement off + sync subset");
const res3 = await td.session({
  brokerId: "9999", userId: "u", password: "p", confirmSettlement: false,
  sync: {
    multipliers: false,
    marginRates: { instrumentIds: ["rb2510"], hedgeFlag: "1" },
  },
});
ok(res3.marginRates === 1, "session executes an explicitly bounded margin-rate synchronization");
ok(res3.identity.frontId === 1 && res3.identity.sessionId === 7 &&
   res3.identity.maxOrderRef === 100 && td.orderRefSeq > res3.identity.maxOrderRef &&
   Object.isFrozen(res3.identity),
  "session returns the immutable MaxOrderRef from this login, not the process high-water mark");
const res4 = await td.session({
  brokerId: "9999", userId: "u", password: "p", confirmSettlement: false,
  sync: {
    multipliers: false,
    marginRates: { instrumentIds: ["rb2510"], hedgeFlag: "2" },
  },
});
ok(res4.marginRates === 1 && sessionMarginHedgeFlags.at(-1) === "2",
  "session synchronizes margin schedules for the explicitly selected hedge flag");
const validSessionLogin = td.reqUserLogin;
td.reqUserLogin = async () => {
  td.native._injectTestLogin(1, 7, "9999", "u");
  return {
    brokerId: "9999", userId: "u", frontId: 1, sessionId: 7,
    maxOrderRef: "not-a-number",
  };
};
await rejects(td.session({
  brokerId: "9999", userId: "u", password: "p", confirmSettlement: false,
  sync: { multipliers: false },
}), "session rejects an invalid correlated MaxOrderRef");
ok(td.broker === undefined && td.investor === undefined,
  "an invalid MaxOrderRef does not leave a usable JS session identity");
td.reqUserLogin = validSessionLogin;

// Exercise the production generated-login context transition (the ordinary
// stubs above intentionally skip it) and the fail-closed multiple-transition
// invariant used to reject a same-call reconnect/login race.
const stableGeneratedTransitionLogin = td.reqUserLogin;
td.reqUserLogin = async () => {
  td.beginRequestContextTransition("simulated generated login");
  td.native._injectTestLogin(1, 7, "9999", "u");
  return { brokerId: "9999", userId: "u", frontId: 1, sessionId: 7 };
};
const generatedTransitionSession = await td.session({
  brokerId: "9999", userId: "u", password: "p", confirmSettlement: false,
  sync: { multipliers: false },
});
ok(generatedTransitionSession.multipliers === 0 &&
   td.broker === "9999" && td.investor === "u",
  "session accepts exactly one generated login context transition");
td.reqUserLogin = () => {
  td.beginRequestContextTransition("simulated first login transition");
  td.beginRequestContextTransition("simulated second login transition");
  return Promise.reject(new Error("superseded login"));
};
await rejects(td.session({
  brokerId: "9999", userId: "u", password: "p", confirmSettlement: false,
  sync: { multipliers: false },
}), "session rejects multiple context transitions crossed by one login request");
td.reqUserLogin = () => {
  td.beginRequestContextTransition("simulated first login transition");
  td.beginRequestContextTransition("simulated second login transition");
  return Promise.resolve({ brokerId: "9999", userId: "u", frontId: 1, sessionId: 7 });
};
let doubleTransitionError;
await td.session({
  brokerId: "9999", userId: "u", password: "p", confirmSettlement: false,
  sync: { multipliers: false },
}).catch((e) => { doubleTransitionError = e; });
ok(/crossed multiple context transitions/.test(String(doubleTransitionError?.message)) &&
   td.broker === undefined,
  "even a successful login response cannot be accepted across multiple context transitions");
td.reqUserLogin = stableGeneratedTransitionLogin;
td.broker = "9999";
td.investor = "u";

// A response and front transition can be delivered in the same native drain.
// The response settles its Promise first, but the compound handshake must not
// resume under the new connection epoch or write the old identity back.
const stableLoginForEpoch = td.reqUserLogin;
const stableSettlementForEpoch = td.reqSettlementInfoConfirm;
let staleSettlementCalls = 0;
td.reqUserLogin = () => new Promise((resolve) => {
  resolve({ brokerId: "9999", userId: "u", frontId: 1, sessionId: 7 });
  td.emit("front-disconnected", 123, {});
});
td.reqSettlementInfoConfirm = async () => { staleSettlementCalls++; return {}; };
await rejects(td.session({
  brokerId: "9999", userId: "u", password: "p",
  sync: { multipliers: false },
}), "session rejects a login response followed by a same-batch disconnect");
ok(staleSettlementCalls === 0 && td.broker === undefined && td.investor === undefined,
  "an invalidated session neither confirms settlement nor resurrects identity");
td.reqUserLogin = stableLoginForEpoch;
td.reqSettlementInfoConfirm = stableSettlementForEpoch;
td.broker = "9999";
td.investor = "u";

// A query result and a front transition can arrive in the same native drain.
// A sync helper must not apply (or retry) a snapshot taken under the old epoch.
const stableInstrumentQueryForEpoch = td.reqQryInstrument;
let crossEpochQueries = 0;
td.reqQryInstrument = () => new Promise((resolve) => {
  crossEpochQueries++;
  resolve([{ instrumentId: "rb2510", productId: "rb", volumeMultiple: 10 }]);
  td.emit("front-disconnected", 124, {});
});
await rejects(td.syncMultipliers(["rb2510"]),
  "syncMultipliers rejects a snapshot followed by a same-batch disconnect");
ok(crossEpochQueries === 1,
  "an invalidated snapshot is neither accepted nor retried across the front change");
td.reqQryInstrument = stableInstrumentQueryForEpoch;
td.broker = "9999";
td.investor = "u";

let instrumentAccessorReads = 0;
const accessorInstrumentIds = new Array(1);
Object.defineProperty(accessorInstrumentIds, 0, {
  configurable: true,
  get() {
    instrumentAccessorReads++;
    return instrumentAccessorReads === 1 ? "rb2510" : "";
  },
});
ok((await td.syncMultipliers(accessorInstrumentIds)) === 1 &&
    instrumentAccessorReads === 1,
  "syncMultipliers snapshots accessor-backed instrument ids before validation can drift");
const successfulLogin = td.reqUserLogin;
td.reqUserLogin = async () => undefined;
await rejects(td.session({
  brokerId: "8888", userId: "u2", password: "p", confirmSettlement: false,
  sync: { multipliers: false },
}), "session fails closed when login omits front/session identity");
ok(td.broker === undefined && td.investor === undefined,
  "an identity-less login revokes the previous confirmed session identity");
// A structurally-valid login whose native handshake was never established (no
// _injectTestLogin) makes _confirmSessionAccount return false -> session rejects.
td.reqUserLogin = async () => ({ brokerId: "9999", userId: "u", frontId: 1, sessionId: 7 });
let confirmFailErr;
await td.session({
  brokerId: "9999", userId: "u", password: "p", confirmSettlement: false,
  sync: { multipliers: false },
}).then(() => {}, (e) => { confirmFailErr = e; });
// Assert the specific reason, not merely that it rejected, so a regression that
// rejects at an earlier gate cannot silently pass this security-critical check.
ok(confirmFailErr && /confirmation failed/i.test(String(confirmFailErr.message ?? confirmFailErr)),
  "session rejects specifically on native account confirmation failure");
ok(td.broker === undefined && td.investor === undefined,
  "a failed native confirmation revokes the confirmed session identity");
td.reqUserLogin = async () => ({
  brokerId: "other", userId: "u2", frontId: 1, sessionId: 7,
});
await rejects(td.session({
  brokerId: "8888", userId: "u2", password: "p", confirmSettlement: false,
  sync: { multipliers: false },
}), "session fails closed when the login response identifies another account");
td.reqUserLogin = successfulLogin;
td.broker = "9999";
td.investor = "u";
{
  const oldCatalog = td.reqQryInstrument, oldMargins = td.reqQryInstrumentMarginRate;
  const requests = [];
  td.reqQryInstrument = async () => [{ instrumentId: 'rb2510', productId: 'rb', volumeMultiple: 10 },
    { instrumentId: 'rb2601', productId: 'rb', volumeMultiple: 10 }, { instrumentId: 'hc2610', productId: 'hc', volumeMultiple: 10 }];
  const rate = (instrumentId, isRelative = 0) => ({ instrumentId, hedgeFlag: '1', isRelative,
    longMarginRatioByMoney: .1, longMarginRatioByVolume: 0, shortMarginRatioByMoney: .1, shortMarginRatioByVolume: 0 });
  td.reqQryInstrumentMarginRate = async q => { requests.push(q.instrumentId); return [rate('rb2601'), rate('rb'), rate('hc2610'), { ...rate('rb'), hedgeFlag: '2' }]; };
  ok(await td.syncMarginRates(['rb2510', 'rb2601']) === 2 && requests.join() === 'rb', 'one product query covers multiple months and filters unrelated rows');
  const snapshot = td.readMarginRates(['rb2510', 'rb2601'], '1');
  ok(snapshot.length === 2 && snapshot[0].longMarginRatioByMoney === .1, 'completed product rates expose per-contract snapshots');
  snapshot[0].longMarginRatioByMoney = 9;
  ok(td.readMarginRates(['rb2510'], '1')[0].longMarginRatioByMoney === .1, 'snapshot caller cannot mutate stored rates');
  td.reqQryInstrumentMarginRate = async () => [rate('rb'), rate('rb2601', 2)];
  ok(await td.syncMarginRates(['rb2510', 'rb2601']) === 1, 'invalid relative flag cannot inherit product schedule');
  throws(() => td.readMarginRates(['rb2510'], '1'), 'incomplete refresh exposes neither stale nor partial product rates');
  td.reqQryInstrumentMarginRate = async () => [rate('rb2601')];
  await rejects(td.syncMarginRates(['rb2510']), 'missing product and contract rows fail closed');
  td.reqQryInstrumentMarginRate = async q => [rate(q.instrumentId)];
  await td.syncMarginRates(['hc2610']);
  td.reqQryInstrument = async () => [{ instrumentId: 'hc2610', productId: 'rb', volumeMultiple: 10 },
    { instrumentId: 'hc2610', productId: 'hc', volumeMultiple: 10 }, { instrumentId: 'rb2510', productId: 'rb', volumeMultiple: 10 }];
  await td.syncMarginRates(['rb2510']);
  ok(td.readMarginRates(['hc2610'], '1')[0].longMarginRatioByMoney === .1,
    'the last catalog row decides product membership and unrelated product snapshots are preserved');
  td.reqQryInstrument = oldCatalog; td.reqQryInstrumentMarginRate = oldMargins;
}
ok((await td.syncMarginRates(["rb2510"])) === 1,
  "syncMarginRates counts usable investor/base schedules");
ok(exchangeMarginQueries === 0,
  "syncMarginRates skips the exchange-base query for an absolute investor row");
td.reqQryInstrumentMarginRate = async () => [{
  instrumentId: "rb2510", hedgeFlag: "1",
  isRelative: 1,
  longMarginRatioByMoney: 0, longMarginRatioByVolume: 0,
  shortMarginRatioByMoney: 0, shortMarginRatioByVolume: 0,
}];
ok((await td.syncMarginRates(["rb2510"])) === 1,
  "syncMarginRates accepts a relative schedule when its exchange base is loaded");
ok(td.readMarginRates(['rb2510'], '1')[0].longMarginRatioByMoney === .08, 'relative snapshot includes exchange base');
ok(exchangeMarginQueries === 1,
  "syncMarginRates queries the exchange base exactly once for a relative row");
td.reqQryInstrumentMarginRate = async () => [{
  instrumentId: "rb2510", hedgeFlag: "1",
  isRelative: 0,
  longMarginRatioByMoney: 0.1, longMarginRatioByVolume: 0,
  shortMarginRatioByMoney: 0.1, shortMarginRatioByVolume: 0,
}, {
  instrumentId: "rb2510", hedgeFlag: "1",
  isRelative: 0,
  longMarginRatioByMoney: Number.MAX_VALUE, longMarginRatioByVolume: 0,
  shortMarginRatioByMoney: 0.1, shortMarginRatioByVolume: 0,
}];
ok((await td.syncMarginRates(["rb2510"])) === 0,
  "syncMarginRates judges the last row of a duplicated instrument, not an earlier usable row");
td.reqQryInstrumentMarginRate = async () => [{
  instrumentId: "rb2510", hedgeFlag: "1", isRelative: 0,
  longMarginRatioByMoney: 0, longMarginRatioByVolume: 0,
  shortMarginRatioByMoney: 0.1, shortMarginRatioByVolume: 0,
}];
ok((await td.syncMarginRates(["rb2510"])) === 0,
  "syncMarginRates rejects a schedule with no positive long-side component");
td.reqQryInstrumentMarginRate = async () => [{
  instrumentId: "rb2510", hedgeFlag: "1", isRelative: 0,
  longMarginRatioByMoney: 0.1, longMarginRatioByVolume: 0,
  shortMarginRatioByMoney: 0, shortMarginRatioByVolume: 0,
}];
ok((await td.syncMarginRates(["rb2510"])) === 0,
  "syncMarginRates rejects a schedule with no positive short-side component");
await rejects(td.syncMarginRates([]), "syncMarginRates rejects an empty universe");
await rejects(td.syncMarginRates(null), "syncMarginRates rejects a non-array bounded universe");
const sparseInstrumentIds = new Array(1);
await rejects(td.syncMarginRates(sparseInstrumentIds),
  "syncMarginRates rejects sparse instrument-id arrays");
await rejects(td.syncMarginRates(["rb2510"], "4"), "syncMarginRates rejects an invalid hedge flag");
await rejects(td.session({ brokerId: "9999", userId: "u", password: "p", appId: "a" }),
  "session rejects a partial authentication pair");
await rejects(td.session({ brokerId: "9999", userId: "u", password: "p", appId: "", authCode: "x" }),
  "session rejects an empty authentication credential pair");
await rejects(td.session({ brokerId: "9999", userId: "u", password: "p", legacySync: {} }),
  "session rejects unknown compatibility fields");
await rejects(td.session(Object.create({ brokerId: "9999", userId: "u", password: "p" })),
  "session rejects inherited credentials / non-plain options");
await rejects(td.session({ brokerId: "9999", userId: "u", password: "p", sync: { position: false } }),
  "session.sync rejects unknown fields");
await rejects(td.session({ brokerId: "9999", userId: "u", password: "p", confirmSettlement: "false" }),
  "session rejects non-boolean confirmSettlement");
for (const removed of ["positions", "orders"]) {
  let selectorError;
  await td.session({ brokerId: "9999", userId: "u", password: "p", sync: { [removed]: false } })
    .catch((e) => { selectorError = e; });
  ok(selectorError instanceof TypeError &&
     selectorError.message === `session.sync: unknown field ${removed}` && td.broker === "9999",
    `session.sync rejects the removed ${removed} selector before changing context`);
}
await rejects(td.session({
  brokerId: "9999", userId: "u", password: "p",
  sync: { marginRates: { instrumentIds: ["rb2510"], hedgeFlag: "4" } },
}), "session rejects an invalid margin-rate hedge flag");
await rejects(td.session({
  brokerId: "9999", userId: "u", password: "p",
  sync: { marginRates: ["rb2510"] },
}), "session rejects the removed implicit speculation margin-rate form");
await rejects(td.session({
  brokerId: "9999", userId: "u", password: "p",
  sync: { marginRates: { instrumentIds: [], hedgeFlag: "1" } },
}), "session rejects an empty explicit margin-rate universe");
await rejects(td.session({
  brokerId: "9999", userId: "u", password: "p",
  sync: { marginRates: { instrumentIds: ["rb2510"], hedgeFlag: "1", legacy: true } },
}), "session rejects unknown explicit margin-rate fields");
await rejects(td.session({ brokerId: "9999", userId: "u", password: "" }),
  "session rejects missing credentials before changing context");
await rejects(td.session({
  brokerId: "9999", userId: "u", password: "p", sync: { multipliers: sparseInstrumentIds },
}), "session rejects sparse instrument-id arrays before issuing a query-all request");
await rejects(td.syncMultipliers(["rb\0evil"]), "syncMultipliers rejects embedded NUL instrument ids");
await rejects(td.syncMultipliers(sparseInstrumentIds),
  "syncMultipliers rejects sparse instrument-id arrays instead of querying all instruments");
const confirmedBroker = td.broker, confirmedInvestor = td.investor;
td.broker = undefined; td.investor = undefined;
await rejects(td.syncMarginRates(["rb2510"]), "syncMarginRates fails closed without a confirmed login");
td.broker = confirmedBroker; td.investor = confirmedInvestor;
// Unsupported-request gate. The handshake tests above revoked the native
// session; confirm it again so the configured rule or the risk mode is the only
// reason a request can be refused, and assert that exact reason.
td.native._injectTestLogin(1, 7, "9999", "u");
td.setRiskLimits({ maxOrderVolume: 1 });
const outcomeOf = (promise) => promise.then(() => null, (e) => e);
const refusedAsUnsupported = (e, reason) => e?.ctpReturnCode === -10006 && e.message ===
  `request is unavailable in the current risk mode or cannot be fully covered by configured local risk controls: ${reason}`;
const localRulesConfigured = "unsupported requests blocked while local risk controls are configured";
const modification = await outcomeOf(td.reqOrderAction({ actionFlag: "3" }));
ok(refusedAsUnsupported(modification, localRulesConfigured),
  `order modification (ActionFlag 3) is refused by the unsupported-request gate while a local rule is configured (${modification?.message})`);
const deletion = await outcomeOf(td.reqOrderAction({ actionFlag: "0" }));
ok(refusedByOfflineVendor(deletion, "order"),
  `order deletion (ActionFlag "0") bypasses the unsupported-request gate and reaches the vendor API (${deletion?.message})`);
await rejects(td.reqOrderAction({ actionFlag: "0", actionFlg: "0" }),
  "generated Trader requests reject unknown fields");
await rejects(td.reqOrderAction({ actionFlag: null }),
  "generated Trader requests reject null fields instead of silently omitting them");
const normalTransfer = await outcomeOf(td.reqFromBankToFutureByFuture({}));
ok(refusedAsUnsupported(normalTransfer, localRulesConfigured),
  `a bank-to-futures transfer is refused by the unsupported-request gate while a local rule is configured (${normalTransfer?.message})`);
td.setRiskLimits({});
const unrestrictedTransfer = await outcomeOf(td.reqFromBankToFutureByFuture({}));
ok(refusedByOfflineVendor(unrestrictedTransfer, "request"),
  `with no local rule a confirmed session forwards the transfer to the vendor API (${unrestrictedTransfer?.message})`);
td.reduceOnly();
const reducedTransfer = await outcomeOf(td.reqFromBankToFutureByFuture({}));
ok(refusedAsUnsupported(reducedTransfer, "opening orders blocked in reduce-only mode"),
  `REDUCE_ONLY refuses a bank-to-futures transfer as a risk-increasing request (${reducedTransfer?.message})`);
td.resume();

// A counter that is not ready is asked again after a pause that faster query
// pacing must not shorten.
{
  const savedInstrumentQuery = td.reqQryInstrument;
  td.queryRetryDelayMs = 250;
  td.queryIntervalMs = 110;
  const sentAt = [];
  td.reqQryInstrument = async () => {
    sentAt.push(performance.now());
    return sentAt.length < 3 ? [] : [{ instrumentId: "rb2510", productId: "rb", volumeMultiple: 10 }];
  };
  await td.syncMultipliers(["rb2510"]);
  ok(sentAt.length === 3 && sentAt[1] - sentAt[0] >= 240 && sentAt[2] - sentAt[1] >= 240,
    "a not-ready query is retried only after the pause");
  sentAt.length = 0;
  td.reqQryInstrument = async () => {
    sentAt.push(performance.now());
    if (sentAt.length === 1) throw Object.assign(new Error("CTP：查询未就绪，请稍后重试"), { errorId: 90 });
    return [{ instrumentId: "rb2510", productId: "rb", volumeMultiple: 10 }];
  };
  await td.syncMultipliers(["rb2510"]);
  ok(sentAt.length === 2 && sentAt[1] - sentAt[0] >= 240,
    "the counter's explicit not-ready reply is asked again after the pause");
  sentAt.length = 0;
  td.reqQryInstrument = async () => { sentAt.push(performance.now()); return []; };
  let pauseError;
  const invalidated = td.syncMultipliers(["rb2510"]).catch((e) => { pauseError = e; });
  setTimeout(() => td.invalidateRequestContext("front disconnected"), 100);
  await invalidated;
  ok(/query synchronization invalidated: front disconnected/.test(String(pauseError?.message)),
    "a front change during the pause rejects the snapshot as invalidated");
  ok(sentAt.length === 1, "a query is never repeated across a front change");
  td.queryRetryDelayMs = 0;
  td.reqQryInstrument = savedInstrumentQuery;
}

// queryWithRetry: required data that stays empty, or keeps failing, throws.
let scopedMultiplierCalls = 0;
td.reqQryInstrument = async () => ++scopedMultiplierCalls === 1
  ? [{ instrumentId: "other", volumeMultiple: 99 }]
  : [{ instrumentId: "rb2510", productId: "rb", volumeMultiple: 10 }];
ok((await td.syncMultipliers(["rb2510"])) === 1 && scopedMultiplierCalls === 2,
  "scoped multiplier sync retries non-target rows instead of accepting them");
td.reqQryInstrument = async () => [
  { instrumentId: "rb2510", productId: "rb", volumeMultiple: 10 },
  { instrumentId: "rb2510", productId: "rb", volumeMultiple: Number.MAX_VALUE },
];
ok((await td.syncMultipliers(["rb2510"])) === 0,
  "syncMultipliers judges only the last row of a duplicated instrument");
td.reqQryInstrument = async () => [];
await rejects(td.syncMultipliers(["rb2510"]), "syncMultipliers rejects when required instrument data stays empty");
td.reqQryInstrument = async () => { throw new Error("flow control"); };
ok((await td.syncMultipliers([])) === 0,
  "syncMultipliers treats an explicit empty universe as a no-op, not query-all");
let threw = false; await td.syncMultipliers().catch(() => { threw = true; });
ok(threw, "syncMultipliers throws after retries when the query keeps failing");
let transientCalls = 0;
td.reqQryInstrument = async () => {
  if (++transientCalls === 1) throw Object.assign(new Error("flow limited"), { ctpReturnCode: -3 });
  return [{ instrumentId: "rb2510", productId: "rb", volumeMultiple: 10 }];
};
ok((await td.syncMultipliers(["rb2510"])) === 1 && transientCalls === 2,
  "sync helpers retry only typed transient CTP flow-control failures");
td.reqQryInstrument = async () => {
  throw Object.assign(new Error("still flow limited"), { ctpReturnCode: -2 });
};
await rejects(td.syncMultipliers(["rb2510"]),
  "sync helpers surface the final transient error after exhausting retries");

// The gate keeps no position/order ledger: once the session is confirmed, an
// in-limit opening order passes every native check with no snapshot at all.
// (The handshake tests above revoked the native session; re-establish it.)
td.setRiskLimits({});
td.native._injectTestLogin(1, 7, "9999", "u");
const plainOrder = { instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500 };
const isRiskSentinel = (e) => e?.ctpReturnCode <= -10001 && e?.ctpReturnCode >= -10009;
const openOutcome = await td.reqOrderInsert(plainOrder).then(() => null, (e) => e);
ok(!isRiskSentinel(openOutcome),
  `a confirmed session admits an in-limit opening order without any ledger synchronization (${openOutcome?.message ?? "sent"})`);
// A login/logout transition still revokes native order authority. Restore the
// JS identity afterwards so validation passes and the native gate decides.
const logoutAttempt = td.reqUserLogout({ brokerId: "9999", userId: "u" });
logoutAttempt.catch(() => {});
td.broker = "9999"; td.investor = "u";
const afterLogout = await td.reqOrderInsert(plainOrder).then(() => null, (e) => e);
ok(afterLogout?.ctpReturnCode === -10001 &&
   /order account does not match confirmed session/.test(afterLogout.message),
  "a login/logout transition revokes native order authority for the previous session");
ok(ledgerQueries === 0, "no handshake in this run queried positions or working orders");

td.close(); md.close(); // release the native doorbell handles so the process exits
ok(td.tradingDay === "", "tradingDay remains a string after close");
console.log(`TRADER-UNIT TEST: ${pass} pass, ${fail} fail`);
if (fail) process.exit(1);
