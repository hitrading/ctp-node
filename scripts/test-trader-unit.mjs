// Coverage for trader.ts logic the e2e tests don't reach offline: the risk/cap
// setters + finite guards, seedFromPositions aggregation, arm() validation, the
// rsp-user-login credential/session listener, and session()/sync* (driven by
// stubbing the leaf req query methods so the row-processing logic runs without a
// live connection). The native risk-engine calls underneath run for real.
import { Trader, MarketData } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-tu", { recursive: true });
mkdirSync("flow-tu-md", { recursive: true });
mkdirSync("flow-tu-signed-login", { recursive: true });
mkdirSync("flow-tu-signed-session", { recursive: true });
mkdirSync("flow-tu-zero-session", { recursive: true });
let pass = 0, fail = 0;
const ok = (c, m) => { c ? (pass++, console.log("  PASS:", m)) : (fail++, console.log("  FAIL:", m)); };
const throws = (fn, m) => { try { fn(); ok(false, m + " (did not throw)"); } catch { ok(true, m); } };
const rejects = async (p, m) => { try { await p; ok(false, m + " (did not reject)"); } catch { ok(true, m); } };

// N-API prototype methods are intentionally immutable. Wrap the native object
// temporarily so the synchronization CAS paths can be forced without adding a
// test-only hook to the production addon.
const withNativeMethod = async (trader, name, replacement, run) => {
  const real = trader.native;
  trader.native = new Proxy(real, {
    get(target, prop) {
      if (prop === name) return (...args) => replacement(target, ...args);
      const value = Reflect.get(target, prop, target);
      return typeof value === "function" ? value.bind(target) : value;
    },
  });
  try { return await run(); }
  finally { trader.native = real; }
};

const td = new Trader("./flow-tu/", "tcp://127.0.0.1:1");
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
]) {
  ok(typeof td[removed] === "undefined", `${removed} compatibility API is absent`);
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
td.setRiskLimits({ maxOrderVolume: 10, maxNotional: 1e6, maxPriceDeviation: 0.02, maxOrdersPerSec: 5, orderBurst: 10, maxCloseOrdersPerSec: 2, closeOrderBurst: 2, maxAccountMargin: 1e7 });
ok(true, "setRiskLimits applies a full config");
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
throws(() => td.setRiskLimits({ maxAccountMargin: Infinity }), "setRiskLimits rejects non-finite maxAccountMargin");
throws(() => td.setRiskLimits({ maxReferenceAgeMs: 0 }), "setRiskLimits rejects a zero reference-age window");
td.setRiskLimits({ maxAccountMargin: 3e5 }); ok(true, "setRiskLimits applies the account-margin hard cap");
td.setInstrumentPositionLimit("rb2510", 100); ok(true, "setInstrumentPositionLimit caps both sides");
td.setInstrumentPositionLimit("ru2510", { long: 100, short: 20 }); ok(true, "setInstrumentPositionLimit supports side limits");
td.setInstrumentPositionLimit("ag2510", { long: 5 }); ok(true, "setInstrumentPositionLimit supports one side");
throws(() => td.setInstrumentPositionLimit("x", Infinity), "setInstrumentPositionLimit rejects non-finite number");
throws(() => td.setInstrumentPositionLimit("x", 1.5), "setInstrumentPositionLimit rejects fractional lots");
throws(() => td.setInstrumentPositionLimit("x", -1), "setInstrumentPositionLimit rejects a negative cap");
throws(() => td.setInstrumentPositionLimit("", 1), "setInstrumentPositionLimit rejects an empty instrument");
throws(() => td.setInstrumentPositionLimit("x", { long: Infinity }), "setInstrumentPositionLimit rejects non-finite side");
throws(() => td.setInstrumentPositionLimit("x", {}), "setInstrumentPositionLimit rejects an empty side object");
throws(() => td.setInstrumentPositionLimit("x", { long: 1, max: 2 }), "setInstrumentPositionLimit rejects unknown fields");
const nullPrototypeSideLimit = Object.assign(Object.create(null), { long: 2 });
td.setInstrumentPositionLimit("np", nullPrototypeSideLimit);
ok(true, "setInstrumentPositionLimit accepts a null-prototype side object");
td.setInstrumentPositionLimits({ a: 5, b: { long: 1, short: 2 } }); ok(true, "setInstrumentPositionLimits atomic batch");
throws(() => td.setInstrumentPositionLimits([]), "setInstrumentPositionLimits rejects arrays");
throws(() => td.setInstrumentPositionLimits(Object.create({ rb: 1 })),
  "setInstrumentPositionLimits rejects inherited map entries");
td.setInstrumentMarginLimit("ag2608", 2e6); ok(true, "setInstrumentMarginLimit");
td.setInstrumentMarginLimits({ au2608: 5e6, ag2608: 2e6 }); ok(true, "setInstrumentMarginLimits atomic batch");
throws(() => td.setInstrumentMarginLimits({ x: Infinity }), "setInstrumentMarginLimits rejects non-finite");
throws(() => td.setInstrumentMarginLimits([]), "setInstrumentMarginLimits rejects arrays");
throws(() => td.setInstrumentMarginLimits(Object.create({ rb: 1 })),
  "setInstrumentMarginLimits rejects inherited map entries");
throws(() => td.setInstrumentMarginLimit("x", -1), "setInstrumentMarginLimit rejects a negative cap");
// A pathological Array *length* on the native limit bindings must be rejected
// with a catchable RangeError BEFORE reserve(); an unbounded reserve() would
// throw std::bad_alloc which — under the Error-only N-API trampoline — escapes
// as an uncatchable std::terminate that aborts the risk-gating process.
const throwsRange = (fn, m) => {
  try { fn(); ok(false, m + " (did not throw)"); }
  catch (e) { ok(e instanceof RangeError && /exceeds the maximum/.test(e.message), m); }
};
throwsRange(() => td.native._setInstrumentPositionLimits(new Array(4294967295)),
  "native _setInstrumentPositionLimits rejects an over-cap array length with a catchable RangeError (no abort)");
throwsRange(() => td.native._setInstrumentMarginLimits(new Array(4294967295)),
  "native _setInstrumentMarginLimits rejects an over-cap array length with a catchable RangeError (no abort)");
const shrinkingPositionLimits = [undefined, { instrumentId: "late", long: 1 }];
Object.defineProperty(shrinkingPositionLimits, 0, {
  configurable: true,
  get() {
    shrinkingPositionLimits.length = 1;
    return { instrumentId: "first", long: 1 };
  },
});
throws(() => td.native._setInstrumentPositionLimits(shrinkingPositionLimits),
  "native position-limit batching rejects an array shortened during its fixed-length snapshot");
const growingMarginLimits = [undefined];
Object.defineProperty(growingMarginLimits, 0, {
  configurable: true,
  get() {
    growingMarginLimits.push(null);
    return { instrumentId: "first", limit: 1 };
  },
});
td.native._setInstrumentMarginLimits(growingMarginLimits);
ok(growingMarginLimits.length === 2,
  "native margin-limit batching ignores rows appended after its fixed-length snapshot");
ok(typeof td.native._seedPositions === "undefined",
  "incremental native _seedPositions compatibility API is absent");
{
  // Capture the seeds handed to native so the JS side->isLong mapping + field
  // pass-through is verified (accountMargin is side-invariant, so the total can't
  // catch an inverted isLong or swapped volume/margin — same bug class as the
  // seedFromPositions test above).
  const seedCap = [];
  const realSeedNative = td.native;
  td.native = new Proxy(realSeedNative, {
    get(target, prop) {
      if (prop === "replacePositions")
        return (seeds, revision) => { seedCap.push(...seeds); return realSeedNative.replacePositions(seeds, revision); };
      const v = Reflect.get(target, prop, target);
      return typeof v === "function" ? v.bind(target) : v;
    },
  });
  td.seedPositions([
    { instrumentId: "rb2510", side: "long", hedgeFlag: "1", volume: 5, margin: 10000 },
    { instrumentId: "au2510", side: "short", hedgeFlag: "3", volume: 2, margin: 20000 },
  ]);
  td.native = realSeedNative;
  const s = (id) => seedCap.find((x) => x.instrumentId === id);
  const rb = s("rb2510"), au = s("au2510");
  ok(seedCap.length === 2 &&
     rb && rb.isLong === true && rb.hedgeFlag === "1" && rb.volume === 5 && rb.margin === 10000 &&
     au && au.isLong === false && au.hedgeFlag === "3" && au.volume === 2 && au.margin === 20000,
    "seedPositions installs each bucket with the correct side->isLong / hedge / volume / margin mapping");
}
throws(() => td.seedPositions(null), "seedPositions rejects a non-array complete snapshot");
await rejects(withNativeMethod(td, "replacePositions", () => false,
  async () => td.seedPositions([])),
"manual seedPositions fails closed when every native revision CAS conflicts");
throws(() => td.seedPositions([
  { instrumentId: "rb", side: "long", hedgeFlag: "1", volume: 1.5, margin: 1 },
]), "seedPositions rejects fractional volume");
throws(() => td.seedPositions([
  { instrumentId: "rb", side: "long", hedgeFlag: "4", volume: 1, margin: 1 },
]), "seedPositions rejects an invalid hedge flag");
throws(() => td.seedPositions([{
  instrumentId: "rb", side: "long", hedgeFlag: "1", volume: 1, margin: 1, legacy: true,
}]), "seedPositions rejects unknown fields");
throws(() => td.seedPositions(new Array(1)), "seedPositions rejects sparse snapshots");
const shrinkingSeeds = [
  { instrumentId: "rb", side: "long", hedgeFlag: "1", volume: 1, margin: 1 },
  { instrumentId: "ag", side: "long", hedgeFlag: "1", volume: 1, margin: 1 },
];
Object.defineProperty(shrinkingSeeds, 0, {
  configurable: true,
  get() {
    shrinkingSeeds.length = 1;
    return { instrumentId: "rb", side: "long", hedgeFlag: "1", volume: 1, margin: 1 };
  },
});
throws(() => td.seedPositions(shrinkingSeeds),
  "seedPositions rejects an accessor that shrinks a complete snapshot during validation");
throws(() => td.seedFromPositions([{ instrumentId: "rb\0evil", posiDirection: "2", hedgeFlag: "1", position: 1, useMargin: 1 }]),
  "seedFromPositions rejects embedded NUL instrument ids");
// Capture the aggregated seeds handed to native so the JS-only aggregation is
// actually verified (accountMargin is invariant under re-bucketing, so the total
// alone can't catch a wrong-bucket / wrong-hedge-split / failed-zero-skip bug).
{
  const seedCapture = [];
  const realSeedNative = td.native;
  td.native = new Proxy(realSeedNative, {
    get(target, prop) {
      if (prop === "replacePositions")
        return (seeds, revision) => { seedCapture.push(...seeds); return realSeedNative.replacePositions(seeds, revision); };
      const v = Reflect.get(target, prop, target);
      return typeof v === "function" ? v.bind(target) : v;
    },
  });
  td.seedFromPositions([
    { instrumentId: "rb", posiDirection: "2", hedgeFlag: "1", position: 3, useMargin: 300 },
    { instrumentId: "rb", posiDirection: "2", hedgeFlag: "1", position: 2, useMargin: 200 },
    { instrumentId: "rb", posiDirection: "2", hedgeFlag: "3", position: 1, useMargin: 50 },
    { instrumentId: "au", posiDirection: "3", hedgeFlag: "1", position: 1, useMargin: 900 },
    { instrumentId: "z", posiDirection: "2", hedgeFlag: "1", position: 0, useMargin: 0 },
  ]);
  td.native = realSeedNative;
  const bucket = (id, isLong, hedge) => seedCapture.find((s) => s.instrumentId === id && s.isLong === isLong && s.hedgeFlag === hedge);
  const rbL1 = bucket("rb", true, "1"), rbL3 = bucket("rb", true, "3"), auS1 = bucket("au", false, "1");
  ok(seedCapture.length === 3 &&
     rbL1 && rbL1.volume === 5 && rbL1.margin === 500 &&   // 3+2 merged
     rbL3 && rbL3.volume === 1 && rbL3.margin === 50 &&     // distinct hedge bucket
     auS1 && auS1.volume === 1 && auS1.margin === 900 &&    // short side kept separate
     !seedCapture.some((s) => s.instrumentId === "z"),      // zero-position row skipped
    "seedFromPositions aggregates by instrument+side+hedge (rb|long|1=5/500, rb|long|3=1/50, au|short|1=1/900) and skips zero rows");
}
throws(() => td.seedFromPositions([{ instrumentId: "rb", posiDirection: "1", hedgeFlag: "1", position: 1, useMargin: 1 }]),
  "seedFromPositions rejects net/unknown direction instead of guessing a side");
throws(() => td.seedFromPositions(null), "seedFromPositions rejects a non-array snapshot");
throws(() => td.seedFromPositions([Object.create({
  instrumentId: "rb", posiDirection: "2", hedgeFlag: "1", position: 1, useMargin: 1,
})]), "seedFromPositions rejects inherited snapshot rows");
throws(() => td.seedFromPositions(new Array(1)),
  "seedFromPositions rejects sparse/inherited array entries");
throws(() => td.seedFromPositions([
  { instrumentId: "overflow", posiDirection: "2", hedgeFlag: "1", position: Number.MAX_SAFE_INTEGER, useMargin: 1 },
  { instrumentId: "overflow", posiDirection: "2", hedgeFlag: "1", position: 1, useMargin: 1 },
]), "seedFromPositions rejects unsafe aggregate volume overflow");
ok(typeof td.accountMargin === "number", "accountMargin property");
td.resetPositions(); ok(true, "resetPositions");
ok(typeof td.apiVersion === "string", "apiVersion property");
ok(typeof td.tradingDay === "string", "tradingDay property");
const stats = td.armStats;
ok(typeof stats.fired === "number" && typeof stats.blocked === "number", "armStats property");

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

// arm() validation branches + a valid arm/disarm.
const md = new MarketData("./flow-tu-md/", "tcp://127.0.0.1:1");
throws(() => td.arm(md, { instrumentId: "", side: "buy", triggerPrice: 1, order: {} }), "arm throws on empty instrumentId");
throws(() => td.arm(md, { instrumentId: "rb", side: "buy", triggerPrice: 0, order: {} }), "arm throws on a non-positive trigger price");
throws(() => td.arm(md, { instrumentId: "rb", side: "buy", triggerPrice: Number.MAX_VALUE, order: {} }), "arm rejects a sentinel-scale trigger price");
throws(() => td.arm(md, { instrumentId: "rb", side: "buy", triggerPrice: 1, order: { instrumentId: "rb" } }), "arm throws on an incomplete order template");
// These target arm()'s OWN side/instrument/direction checks, which run only
// after validateOrder() passes — so the templates must be otherwise valid
// (incl. limitPrice), else validateOrder throws first and arm's checks never run.
throws(() => td.arm(md, { instrumentId: "rb", side: "buy", triggerPrice: 1, order: { instrumentId: "rb", direction: "1", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500 } }), "arm requires trigger side to match order direction");
throws(() => td.arm(md, { instrumentId: "rb", side: "buy", triggerPrice: 1, order: { instrumentId: "rb", direction: "0", combOffsetFlag: "30", volumeTotalOriginal: 1 } }), "arm rejects a multi-leg offset template");
throws(() => td.arm(md, { instrumentId: "rb", side: "hold", triggerPrice: 1, order: { instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500 } }),
  "arm rejects an unknown trigger side");
throws(() => td.arm(md, { instrumentId: "rb", side: "buy", triggerPrice: 1, order: { instrumentId: "ag", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500 } }),
  "arm rejects a trigger/order instrument mismatch");
td.haltAll();
throws(() => td.arm(md, { instrumentId: "rb", side: "buy", triggerPrice: 1,
  order: { instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 1 } }),
  "HALT_ALL rejects new arms instead of preserving them for resume");
td.resume();
const h = td.arm(md, { instrumentId: "rb", side: "sell", triggerPrice: 3500, order: { instrumentId: "rb", direction: "1", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500 } });
ok(typeof h.id === "number", "arm returns a handle id");
ok(typeof h.disarm() === "boolean", "arm handle disarm()");

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
  tdTrack.close(); mdTrack.close();
}

const signedSessionTrader = new Trader("./flow-tu-signed-session/", "tcp://127.0.0.1:1");
signedSessionTrader.reqUserLogin = async () => {
  const sessionId = -2147483647;
  ok(signedSessionTrader.native._injectTestLogin(1, sessionId, "9999", "signed"),
    "native login/account confirmation accepts a negative non-zero SessionID");
  return {
    brokerId: "9999", userId: "signed", maxOrderRef: "0", frontId: 1, sessionId,
  };
};
const signedSession = await signedSessionTrader.session({
  brokerId: "9999", userId: "signed", password: "p", confirmSettlement: false,
  sync: { multipliers: false, positions: false, orders: false },
});
ok(signedSession.identity.sessionId === -2147483647,
  "session() preserves the exact negative signed SessionID");
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
td.reqQryInvestorPosition = async () => [
  { brokerId: "9999", investorId: "u", instrumentId: "rb2510", posiDirection: "2", hedgeFlag: "1", position: 3, useMargin: 30000 },
];
td.reqQryOrder = async () => [
  { brokerId: "9999", investorId: "u", frontId: 1, sessionId: 7, orderRef: "7", instrumentId: "rb2510", combOffsetFlag: "0", combHedgeFlag: "1", direction: "0", orderStatus: "3", limitPrice: 3500, volumeTotalOriginal: 2, volumeTraded: 0 },
  { brokerId: "9999", investorId: "u", frontId: 1, sessionId: 7, orderRef: "8", instrumentId: "rb", combOffsetFlag: "1", combHedgeFlag: "1", direction: "0", orderStatus: "0", limitPrice: 1, volumeTotalOriginal: 1, volumeTraded: 1 },
  { brokerId: "9999", investorId: "u", frontId: 2, sessionId: 9, orderRef: "9", instrumentId: "mix", combOffsetFlag: "0", combHedgeFlag: "1", direction: "0", orderStatus: "3", limitPrice: 1, volumeTotalOriginal: 1, volumeTraded: 0 },
];
const res = await td.session({ brokerId: "9999", userId: "u", password: "p", appId: "a", authCode: "c" });
ok(res.multipliers === 1, "session: syncMultipliers applied 1 (skipped volumeMultiple 0)");
ok(res.positions === 1, "session: syncPositions counted 1");
ok(res.orders === 2, "session: syncOrders re-reserved two single-leg working opens");
const res2 = await td.session({ brokerId: "9999", userId: "u", password: "p", confirmSettlement: false, sync: { multipliers: ["rb2510"], positions: false, orders: false } });
ok(res2.multipliers === 1 && res2.marginRates === 0 && res2.positions === 0 && res2.orders === 0, "session: auth skipped + settlement off + sync subset");
const res3 = await td.session({
  brokerId: "9999", userId: "u", password: "p", confirmSettlement: false,
  sync: {
    multipliers: false,
    marginRates: { instrumentIds: ["rb2510"], hedgeFlag: "1" },
    positions: false,
    orders: false,
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
    positions: false,
    orders: false,
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
  sync: { multipliers: false, positions: false, orders: false },
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
  sync: { multipliers: false, positions: false, orders: false },
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
  sync: { multipliers: false, positions: false, orders: false },
}), "session rejects multiple context transitions crossed by one login request");
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
  sync: { multipliers: false, positions: false, orders: false },
}), "session rejects a login response followed by a same-batch disconnect");
ok(staleSettlementCalls === 0 && td.broker === undefined && td.investor === undefined,
  "an invalidated session neither confirms settlement nor resurrects identity");
td.reqUserLogin = stableLoginForEpoch;
td.reqSettlementInfoConfirm = stableSettlementForEpoch;
td.broker = "9999";
td.investor = "u";

const stablePositionQueryForEpoch = td.reqQryInvestorPosition;
let crossEpochPositionCommits = 0;
td.reqQryInvestorPosition = () => new Promise((resolve) => {
  resolve([{
    brokerId: "9999", investorId: "u", instrumentId: "rb2510",
    posiDirection: "2", hedgeFlag: "1", position: 3, useMargin: 30000,
  }]);
  td.emit("front-disconnected", 124, {});
});
await rejects(withNativeMethod(td, "replacePositions", () => {
  crossEpochPositionCommits++;
  return true;
}, () => td.syncPositions()),
"syncPositions rejects a snapshot followed by a same-batch disconnect");
ok(crossEpochPositionCommits === 0,
  "an invalidated position snapshot never reaches the native CAS commit");
td.reqQryInvestorPosition = stablePositionQueryForEpoch;
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
  sync: { multipliers: false, positions: false, orders: false },
}), "session fails closed when login omits front/session identity");
ok(td.broker === undefined && td.investor === undefined,
  "an identity-less login revokes the previous confirmed session identity");
// A structurally-valid login whose native handshake was never established (no
// _injectTestLogin) makes _confirmSessionAccount return false -> session rejects.
td.reqUserLogin = async () => ({ brokerId: "9999", userId: "u", frontId: 1, sessionId: 7 });
let confirmFailErr;
await td.session({
  brokerId: "9999", userId: "u", password: "p", confirmSettlement: false,
  sync: { multipliers: false, positions: false, orders: false },
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
  sync: { multipliers: false, positions: false, orders: false },
}), "session fails closed when the login response identifies another account");
td.reqUserLogin = successfulLogin;
td.broker = "9999";
td.investor = "u";
const validPositionQuery = td.reqQryInvestorPosition;
td.reqQryInvestorPosition = async () => [{
  brokerId: "other", investorId: "other", instrumentId: "rb2510",
  posiDirection: "2", hedgeFlag: "1", position: 3, useMargin: 30000,
}];
await rejects(td.syncPositions(),
  "syncPositions rejects a non-empty snapshot for a different account instead of treating it as flat");
td.reqQryInvestorPosition = validPositionQuery;
const validOrderQuery = td.reqQryOrder;
td.reqQryOrder = async () => [{
  brokerId: "other", investorId: "other", frontId: 1, sessionId: 7,
  orderRef: "foreign", tradingDay: "20260710", exchangeId: "SHFE",
  orderSysId: "foreign", instrumentId: "rb2510", combOffsetFlag: "0",
  combHedgeFlag: "1", direction: "0", orderStatus: "3", limitPrice: 3500,
  volumeTotalOriginal: 1, volumeTraded: 0,
}];
await rejects(td.syncOrders(),
  "syncOrders rejects a non-empty snapshot for a different account instead of clearing reservations");
td.reqQryOrder = validOrderQuery;
// Restore both authoritative ledgers for the tests below. Order sync
// deliberately keeps positions uncertain until the final position snapshot.
await td.syncOrders();
await td.syncPositions();
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
  "syncMarginRates reports the final native schedule state, not an earlier usable row");
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
await rejects(td.session({ brokerId: "9999", userId: "u", password: "p", sync: { positions: "yes" } }),
  "session rejects a non-boolean position-sync selector");
await rejects(td.session({ brokerId: "9999", userId: "u", password: "p", sync: { orders: 1 } }),
  "session rejects a non-boolean order-sync selector");
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
await rejects(td.syncPositions({ brokerId: "other", investorId: "other" }),
  "syncPositions rejects the removed account-override interface");
await rejects(td.syncOrders({ brokerId: "other", investorId: "other" }),
  "syncOrders rejects the removed account-override interface");
const confirmedBroker = td.broker, confirmedInvestor = td.investor;
td.broker = undefined; td.investor = undefined;
await rejects(td.syncMarginRates(["rb2510"]), "syncMarginRates fails closed without a confirmed login");
await rejects(td.syncPositions(), "syncPositions fails closed without a confirmed login");
await rejects(td.syncOrders(), "syncOrders fails closed without a confirmed login");
td.broker = confirmedBroker; td.investor = confirmedInvestor;
td.setRiskLimits({ maxOrderVolume: 1 });
let modifyBlocked = false;
await td.reqOrderAction({ actionFlag: "3" }).catch((e) => {
  modifyBlocked = e?.ctpReturnCode === -10006 ||
    /cannot be fully covered|unavailable in the current risk mode/i.test(String(e));
});
ok(modifyBlocked, "order modification (ActionFlag 3) is routed through the fail-closed unsupported-risk gate (-10006)");
let deleteBlockedByRisk = false;
await td.reqOrderAction({ actionFlag: "0" }).catch((e) => {
  // The real unsupported-risk rejection is CTP code -10006 (client.ts
  // riskRejectMessage). Match its actual message / code so a regression that
  // wrongly routed a Delete (ActionFlag "0") through the risk gate is caught —
  // the old /unsupported risk path/ text exists nowhere and never matched.
  deleteBlockedByRisk =
    e?.ctpReturnCode === -10006 ||
    /cannot be fully covered|unavailable in the current risk mode/i.test(String(e));
});
ok(!deleteBlockedByRisk, "order deletion (ActionFlag \"0\") is not gated by the unsupported-risk path");
await rejects(td.reqOrderAction({ actionFlag: "0", actionFlg: "0" }),
  "generated Trader requests reject unknown fields");
await rejects(td.reqOrderAction({ actionFlag: null }),
  "generated Trader requests reject null fields instead of silently omitting them");
let normalTransferBlocked = false;
await td.reqFromBankToFutureByFuture({}).catch((e) => {
  normalTransferBlocked = e?.ctpReturnCode === -10006 ||
    /cannot be fully covered|unavailable in the current risk mode/i.test(String(e));
});
ok(normalTransferBlocked, "a bank-to-futures transfer is routed through the fail-closed unsupported-risk gate (-10006)");
td.reduceOnly();
let transferBlocked = false;
await td.reqFromBankToFutureByFuture({}).catch((e) => {
  transferBlocked = /unsupported|risk/i.test(String(e));
});
ok(transferBlocked, "bank-to-futures transfer is explicitly blocked outside NORMAL mode");
td.resume();

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
  "syncMultipliers reports the final native multiplier state for duplicate rows");
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

td.setRiskLimits({});
await td.syncPositions(); // res2 deliberately skipped sync and left the position ledger fail-closed

let replacePositionAttempts = 0;
const positionsAfterConflict = await withNativeMethod(td, "replacePositions", (native, ...args) =>
  ++replacePositionAttempts === 1 ? false : native.replacePositions(...args),
() => td.syncPositions());
ok(positionsAfterConflict === 1 && replacePositionAttempts === 2,
  "syncPositions retries a snapshot invalidated by a live ledger revision");
await rejects(withNativeMethod(td, "replacePositions", () => false, () => td.syncPositions()),
  "syncPositions fails closed when every snapshot revision conflicts");
await td.syncPositions(); // restore a certain held-position snapshot after the forced failure

let rebuildOrderAttempts = 0;
const ordersAfterConflict = await withNativeMethod(td, "rebuildReservations", (native, ...args) =>
  ++rebuildOrderAttempts === 1 ? false : native.rebuildReservations(...args),
() => td.syncOrders());
ok(ordersAfterConflict === 2 && rebuildOrderAttempts === 2,
  "syncOrders retries a snapshot invalidated by a live ledger revision");
await rejects(withNativeMethod(td, "rebuildReservations", () => false, () => td.syncOrders()),
  "syncOrders fails closed when every snapshot revision conflicts");
await td.syncOrders();
await td.syncPositions(); // order-first reconciliation intentionally leaves positions uncertain

// The epoch/disconnect tests above invalidated the native session; re-establish
// a confirmed one so the unsupported-order gate below reaches the ledger check
// (rather than failing at the account gate) on a live account.
td.native._injectTestLogin(1, 7, "9999", "u");
await td.syncPositions();
td.setInstrumentPositionLimit("rb", 10);
td.reqQryOrder = async () => [{
  brokerId: "9999", investorId: "u", frontId: 1, sessionId: 7, orderRef: "9",
  orderSysId: "sys-x", instrumentId: "rb", combOffsetFlag: "0", combHedgeFlag: "1",
  direction: "0", orderStatus: "3", limitPrice: 3500, volumeTotalOriginal: 1, volumeTraded: 0,
}]; // orderSysId present but exchangeId empty -> malformed working-order snapshot
await rejects(td.syncOrders(), "syncOrders rejects a malformed working-order snapshot");
td.reqQryOrder = async () => [{
  brokerId: "9999", investorId: "u", frontId: 2, sessionId: 9, orderRef: "multi-live", tradingDay: "20260710",
  exchangeId: "SHFE", orderSysId: "sys-multi", instrumentId: "rb",
  combOffsetFlag: "30", combHedgeFlag: "11", direction: "0", orderStatus: "3",
  limitPrice: 3500, volumeTotalOriginal: 1, volumeTraded: 0,
}];
await rejects(td.syncOrders(), "syncOrders reports an unsupported working multi-leg order");
let unsupportedBlocked = false;
await td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, orderRef: "2001" })
  .catch((e) => { unsupportedBlocked = /metadata unavailable/i.test(String(e)); });
ok(unsupportedBlocked, "unsupported working orders make position-dependent opens fail closed");
td.reqQryOrder = async () => [];
ok((await td.syncOrders()) === 0, "a later authoritative empty order snapshot clears unsupported state");
let orderFirstStillBlocked = false;
await td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, orderRef: "2002" })
  .catch((e) => { orderFirstStillBlocked = /metadata unavailable/i.test(String(e)); });
ok(orderFirstStillBlocked, "syncOrders keeps opens blocked until a later position snapshot");
await td.syncPositions();
let stillMetadataBlocked = false;
await td.reqOrderInsert({ instrumentId: "rb", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, orderRef: "2003" })
  .catch((e) => { stillMetadataBlocked = /metadata unavailable/i.test(String(e)); });
ok(!stillMetadataBlocked, "opens no longer fail on unsupported-order ambiguity after reconciliation");

const sessionBoundArm = td.arm(md, { instrumentId: "rb", side: "sell", triggerPrice: 3500, order: { instrumentId: "rb", direction: "1", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500 } });
const logoutAttempt = td.reqUserLogout({ brokerId: "9999", userId: "u" });
logoutAttempt.catch(() => {});
ok(sessionBoundArm.disarm() === false,
  "a login/logout transition revokes arms from the previous session");

// Regression (round-17 finding): the syncOrders() resync key MUST be byte-
// identical to the native realtime key built in traderspi.gen.cc (OnRtnOrder/
// OnRtnTrade): day|broker|investor|exchange|instrument|orderSysId. A 4-field key
// (missing broker/investor) leaves a reservation rebuilt on reconnect
// uncorrelated with the subsequent realtime order/trade callbacks, stranding
// opening inserts fail-closed for any account carrying resting orders.
{
  mkdirSync("flow-tu-syskey", { recursive: true });
  const tdKey = new Trader("./flow-tu-syskey/", "tcp://127.0.0.1:1");
  tdKey.broker = "9999";
  tdKey.investor = "u";
  const captured = [];
  const realKeyNative = tdKey.native;
  tdKey.native = new Proxy(realKeyNative, {
    get(target, prop) {
      if (prop === "rebuildReservations")
        return (working) => { captured.push(...working); return true; }; // capture only, no native mutation
      const v = Reflect.get(target, prop, target);
      return typeof v === "function" ? v.bind(target) : v;
    },
  });
  tdKey.reqQryOrder = async () => [{
    brokerId: "9999", investorId: "u", frontId: 1, sessionId: 7, orderRef: "syskey-probe",
    instrumentId: "rb2510", combOffsetFlag: "0", combHedgeFlag: "1", direction: "0",
    orderStatus: "3", limitPrice: 3500, volumeTotalOriginal: 2, volumeTraded: 0,
    tradingDay: "20260711", exchangeId: "SHFE", orderSysId: "800001",
  }];
  await tdKey.syncOrders();
  tdKey.native = realKeyNative;
  const probe = captured.find((w) => w.orderRef === "syskey-probe");
  ok(probe && probe.orderSysKey === "20260711|9999|u|SHFE|rb2510|800001",
    "syncOrders resync key is byte-identical to the native realtime key (day|broker|investor|exchange|instrument|orderSysId)");
  tdKey.close();
}

td.close(); md.close(); // release the native doorbell handles so the process exits
throws(() => td._armRegistry(), "closed Trader cannot expose a dead arm registry");
ok(td.tradingDay === "", "tradingDay remains a string after close");
console.log(`TRADER-UNIT TEST: ${pass} pass, ${fail} fail`);
if (fail) process.exit(1);
