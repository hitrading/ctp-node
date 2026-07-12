// Coverage for client.ts request/response machinery that a live connection would
// normally drive. TS access modifiers don't exist at runtime, so we drive the
// real methods (request/submit/deliver/armRequestTimer) directly with stub send
// callbacks and crafted decoded records — deterministic, no network. A MarketData
// is the vehicle (its zeroIdFallbackEvents includes the login event).
import { mkdirSync } from "node:fs";
process.env.CTP_RECREATE_WARN = "invalid-threshold"; // exercise fallback parsing
const { MarketData, CtpClient } = await import("../dist/index.js");
const { native } = await import("../dist/native-binding.js");
const { STRUCT_ID } = await import("../dist/generated/structs.gen.js");

const recreateWarnings = [];
const realWarn = console.warn;
console.warn = (...args) => {
  if (String(args[0]).includes("[ctp-node]")) recreateWarnings.push(args);
  else realWarn(...args);
};

mkdirSync("flow-cu", { recursive: true });
mkdirSync("flow-cu-close", { recursive: true });
let pass = 0, fail = 0;
const ok = (c, m) => { c ? (pass++, console.log("  PASS:", m)) : (fail++, console.log("  FAIL:", m)); };
const msgOf = (e) => String((e && e.message) || e);
const rejects = async (p, frag, m) => {
  try { await p; ok(false, m + " (did not reject)"); }
  catch (e) { ok(!frag || msgOf(e).includes(frag), m + (frag && !msgOf(e).includes(frag) ? ` (got: ${msgOf(e)})` : "")); }
};
const resolves = async (p, m) => { try { await p; ok(true, m); } catch (e) { ok(false, m + ` (rejected: ${msgOf(e)})`); } };

const c = new MarketData("./flow-cu/", "tcp://127.0.0.1:1");

// Non-ASCII request fields take the native GB18030 path, while ASCII stays on
// the allocation-light JS path.
ok(c.encode(STRUCT_ID.QryMulticastInstrument, { instrumentId: "中文" }) instanceof Uint8Array,
  "request encoder handles non-ASCII fields through GB18030");

// --- request(): non-zero send codes route through riskRejectMessage; throws/timeout ---
await rejects(c.request(() => -10001), "pre-trade risk", "request -10001 -> pre-trade risk");
await rejects(c.request(() => -10002), "rate limited", "request -10002 -> rate limited");
await rejects(c.request(() => -10003), "position margin limit", "request -10003 -> position margin");
await rejects(c.request(() => -10004), "max position volume", "request -10004 -> position volume");
await rejects(c.request(() => -10005), "duplicate orderRef", "request -10005 -> duplicate orderRef");
await rejects(c.request(() => -10006), "cannot be fully covered", "request -10006 -> unsupported risk path");
await rejects(c.request(() => -10007), "risk state or metadata unavailable", "request -10007 -> unavailable risk state/metadata");
await rejects(c.request(() => -10008), "invalid order", "request -10008 -> invalid order fields");
await rejects(c.request(() => -10009), "session transition", "request -10009 -> ambiguous session transition");
await rejects(c.request(() => -1), "code -1", "request generic non-zero code");
let flowCode;
await c.request(() => -3).catch((e) => { flowCode = e.ctpReturnCode; });
ok(flowCode === -3, "send rejections expose the native CTP return code");
await rejects(c.request(() => { throw new Error("send boom"); }), "send boom", "request rejects when send() throws");

// --- submit(): resolve on 0, reject on non-zero / throw ---
await resolves(c.submit(() => 0), "submit resolves on rc 0");
await rejects(c.submit(() => -1), "code -1", "submit rejects on non-zero rc");
await rejects(c.submit(() => { throw new Error("submit boom"); }), "submit boom", "submit rejects when send() throws");

// --- deliver(): request/response correlation ---
const mk = (resolve, reject, single) => ({ rows: [], resolve, reject, single });
let r1; const id1 = c.nextRequestId(); c.pending.set(id1, mk((v) => { r1 = v; }, () => {}, true));
c.deliver({ evt: 0x1004, value: { a: 1 }, options: { requestId: id1, isLast: true } });
ok(r1 && r1.a === 1, "deliver resolves a single-row request by id");

const originalResolvedHook = c.onRequestResolved.bind(c);
let hookError;
c.onRequestResolved = () => { throw new Error("resolved hook failed"); };
const hookId = c.nextRequestId();
c.pending.set(hookId, mk(() => {}, (error) => { hookError = error; }, true));
c.deliver({ evt: 0x1004, value: {}, options: { requestId: hookId, isLast: true } });
c.onRequestResolved = originalResolvedHook;
ok(/resolved hook failed/.test(msgOf(hookError)),
  "an internal resolved-response hook failure rejects instead of stranding its request");

let r2; const id2 = c.nextRequestId(); c.pending.set(id2, mk((v) => { r2 = v; }, () => {}, false));
c.deliver({ evt: 0x1006, value: { n: 1 }, options: { requestId: id2, isLast: false } });
c.deliver({ evt: 0x1006, value: { n: 2 }, options: { requestId: id2, isLast: true } });
ok(Array.isArray(r2) && r2.length === 2, "deliver accumulates multi-row, then resolves on isLast");

let e3; const id3 = c.nextRequestId(); c.pending.set(id3, mk(() => {}, (er) => { e3 = er; }, true));
c.deliver({ evt: 0x1007, value: undefined, options: { requestId: id3, isLast: true, rspInfo: { errorId: 5, errorMsg: "bad" } } });
ok(e3 && e3.errorId === 5, "deliver rejects on an rspInfo error response");

let r4; const id4 = c.nextRequestId(); c.pending.set(id4, { ...mk((v) => { r4 = v; }, () => {}, true), zeroIdEvent: 0x1004 });
c.deliver({ evt: 0x1004, value: { z: 9 }, options: { requestId: undefined, isLast: true } });
ok(r4 && r4.z === 9, "deliver zero-id fallback resolves the matching MD login");

let loginResolved = false, logoutResolved = false;
const loginId = c.nextRequestId(), logoutId = c.nextRequestId();
c.pending.set(loginId, { ...mk(() => { loginResolved = true; }, () => {}, true), zeroIdEvent: 0x1004 });
c.pending.set(logoutId, { ...mk(() => { logoutResolved = true; }, () => {}, true), zeroIdEvent: 0x1005 });
c.deliver({ evt: 0x1005, value: {}, options: { requestId: undefined, isLast: true } });
ok(!loginResolved && logoutResolved,
  "zero-id fallback cannot let a concurrent logout consume the login request");
c.pending.delete(loginId);

const unmatchedId = c.nextRequestId();
c.pending.set(unmatchedId, mk(() => {}, () => {}, true));
c.deliver({ evt: 0x1004, value: {}, options: { requestId: unmatchedId + 1, isLast: true } });
ok(c.pending.has(unmatchedId), "an unmatched response cannot settle another pending request");
c.pending.delete(unmatchedId);

let fallbackError; const fallbackId = c.nextRequestId();
c.pending.set(fallbackId, {
  ...mk(() => {}, (e) => { fallbackError = e; }, true),
  timer: setTimeout(() => {}, 1000),
});
c.deliver({ evt: 0x1007, value: undefined, options: {
  requestId: fallbackId, isLast: true, rspInfo: { errorId: 77, errorMsg: "" },
} });
ok(/CTP error 77/.test(msgOf(fallbackError)),
  "an empty response error message uses the numeric CTP fallback and clears its timer");

let emitted = false; c.on("rtn-depth-market-data", () => { emitted = true; });
c.deliver({ evt: 0x100c, value: { t: 1 }, options: { requestId: undefined, isLast: undefined } });
ok(emitted, "deliver emits a streaming push without correlation");

let unknown = false; c.on("event:39321", () => { unknown = true; });
c.deliver({ evt: 0x9999, value: undefined, options: { requestId: undefined, isLast: undefined } });
ok(unknown, "deliver emits event:<id> for an unmapped event code");

// A native provider may deliver an inline callback before send() returns. The
// settled request must not leave a dead idle timer behind.
let inlineTimerArms = 0;
const realArmRequestTimer = c.armRequestTimer.bind(c);
c.armRequestTimer = (...args) => { inlineTimerArms++; return realArmRequestTimer(...args); };
const inline = await c.request((id) => {
  c.deliver({ evt: 0x1004, value: { inline: true }, options: { requestId: id, isLast: true } });
  return 0;
}, true);
c.armRequestTimer = realArmRequestTimer;
ok(inline?.inline === true && inlineTimerArms === 0,
  "an inline response settles without arming a dead request timeout");

// --- idle timeout + the timeout<=0 no-op ---
c.requestTimeoutMs = 10;
await rejects(c.request(() => 0), "timed out", "request idle timeout rejects");
c.requestTimeoutMs = 0; const noArm = mk(() => {}, () => {}, true); c.armRequestTimer(987654, noArm);
ok(noArm.timer === undefined, "armRequestTimer is a no-op when timeout<=0 (no idle timer armed)");
c.requestTimeoutMs = 30000;
const rearmed = mk(() => {}, () => {}, true);
rearmed.timer = setTimeout(() => {}, 1000);
const oldTimer = rearmed.timer;
c.armRequestTimer(987655, rearmed);
ok(rearmed.timer !== oldTimer, "armRequestTimer replaces an existing idle timer (arms a fresh one)");
clearTimeout(rearmed.timer); clearTimeout(oldTimer);
// base default (subclasses override it) + the timer callback's throwing-hook and
// shouldTimeout=false re-arm branches.
ok(CtpClient.prototype.shouldTimeoutRequest.call(c, 1, false) === true,
  "base shouldTimeoutRequest defaults to timing out");
c.requestTimeoutMs = 5;
let timerErr; const origHook = c.shouldTimeoutRequest;
c.shouldTimeoutRequest = () => { throw new Error("timeout-hook boom"); };
const throwId = c.nextRequestId();
c.pending.set(throwId, mk(() => {}, (e) => { timerErr = e; }, true));
c.armRequestTimer(throwId, c.pending.get(throwId));
await new Promise((r) => setTimeout(r, 25));
ok(/timeout-hook boom/.test(msgOf(timerErr)) && !c.pending.has(throwId),
  "a throwing shouldTimeoutRequest rejects and drops the pending request");
c.shouldTimeoutRequest = () => false;
let reArms = 0; const realArm = c.armRequestTimer.bind(c);
c.armRequestTimer = (...a) => { reArms++; return realArm(...a); };
const keepId = c.nextRequestId();
c.pending.set(keepId, mk(() => {}, () => {}, true));
c.armRequestTimer(keepId, c.pending.get(keepId));
await new Promise((r) => setTimeout(r, 25));
c.armRequestTimer = realArm; c.pending.delete(keepId);
c.shouldTimeoutRequest = origHook; c.requestTimeoutMs = 30000;
ok(reArms >= 2, "shouldTimeout=false re-arms and keeps waiting for the lossless response");

// --- rejectAllPending via front-disconnected ---
let dj; const idd = c.nextRequestId();
c.pending.set(idd, { ...mk(() => {}, (er) => { dj = er; }, true), timer: setTimeout(() => {}, 1000) });
c.emit("front-disconnected", 0, {});
ok(dj && /disconnect/.test(msgOf(dj)), "front-disconnected rejects all pending requests");

// --- JS ids stay below the upper-half range reserved for native armed orders ---
c.reqSeq = 0x3fffffff;
ok(c.nextRequestId() === 1,
  "nextRequestId wraps before the native armed-order id range");

// Every generated ReqQry* / ReqQuery* uses this scheduler, so concurrent queries
// cannot bypass CTP's process/client flow-control interval.
const querySent = [];
const scheduledQuery = () => c.queryRequest((id) => {
  querySent.push(Date.now());
  queueMicrotask(() => c.deliver({
    evt: 0x1006, value: { id }, options: { requestId: id, isLast: true },
  }));
  return 0;
});
const t0 = Date.now();
await Promise.all([scheduledQuery(), scheduledQuery()]);
ok(querySent.length === 2 && querySent[1] - querySent[0] >= 1000,
  "queryRequest serializes concurrent queries at the global flow-control interval");
// Bound the FIRST send against a pre-dispatch baseline (the old `querySent[0]>0`
// was a Date.now() value, always true — a wrongly-delayed first query passed it).
// 500ms is well under the 1100ms interval, so it distinguishes immediate from delayed.
ok(querySent[0] - t0 < 500, "the first query is eligible immediately without a startup delay");

// A connection/session epoch change cancels both a spacing wait and its queued
// tail before either can reach native code.
c.lastQuerySentAt = performance.now();
let staleSends = 0;
const stale1 = c.queryRequest(() => { staleSends++; return 0; });
const stale2 = c.queryRequest(() => { staleSends++; return 0; });
await Promise.resolve();
c.emit("front-disconnected", 123, {});
const stale = await Promise.allSettled([stale1, stale2]);
ok(stale.every((x) => x.status === "rejected") && staleSends === 0,
  "a front epoch change rejects unsent queued queries instead of crossing sessions");

const savedScheduled = c.scheduledQueries;
c.scheduledQueries = 256;
await rejects(c.queryRequest(() => 0), "queue is full",
  "the query scheduler rejects an unbounded Promise backlog");
c.scheduledQueries = savedScheduled;

// --- decodeSlot / dispatch over synthetic slots written into the ring buffer.
//     decodeSlot is a pure read; offline the producer never writes, so composing
//     a slot by hand and decoding it is safe and deterministic. ---
const info = c.native._info();
const ss = info.slotSize, hs = info.headerSize, dv = c.view, u8 = c.u8;
const writeSlot = (idx, { evt, reqId = 0, isLast = -1, errId = 0, sid = -1, plen = 0, i32, errMsg }) => {
  const o = idx * ss;
  for (let k = 0; k < ss; k++) u8[o + k] = 0;
  dv.setInt32(o + 0, evt, true); dv.setInt32(o + 4, reqId, true); dv.setInt32(o + 8, isLast, true);
  dv.setInt32(o + 12, errId, true); dv.setInt32(o + 16, sid, true); dv.setInt32(o + 20, plen, true);
  if (errMsg) for (let k = 0; k < errMsg.length; k++) u8[o + 24 + k] = errMsg.charCodeAt(k);
  if (i32 !== undefined) dv.setInt32(o + hs, i32, true);
};
writeSlot(0, { evt: 0x1002, sid: -2, plen: 4, i32: 42 });
ok(c.decodeSlot(0 * ss).value === 42, "decodeSlot reads a raw int32 payload (sid === -2)");
writeSlot(1, { evt: 0x1007, errId: 9, errMsg: "boom" });
const d1 = c.decodeSlot(1 * ss);
ok(d1.options.rspInfo && d1.options.rspInfo.errorId === 9 && d1.options.rspInfo.errorMsg === "boom", "decodeSlot builds rspInfo when errId !== 0");
writeSlot(2, { evt: 0x1002, sid: 99999, plen: 4, i32: 1 });
ok(c.decodeSlot(2 * ss).value === undefined, "decodeSlot yields undefined for an out-of-range sid (torn-read guard)");
writeSlot(2, { evt: 0x1002, sid: 0, plen: 1 });
ok(c.decodeSlot(2 * ss).value === undefined,
  "decodeSlot rejects a payload shorter than its declared struct layout");
let dval; c.on("front-disconnected", (v) => { dval = v; });
writeSlot(3, { evt: 0x1002, sid: -2, plen: 4, i32: 7 });
c.dispatch(3 * ss);
ok(dval === 7, "dispatch decodes a slot and delivers it");
writeSlot(3, { evt: 0x1002, sid: -2, plen: 4, i32: 8, isLast: 0 });
ok(c.decodeSlot(3 * ss).options.isLast === false,
  "decodeSlot preserves a non-final response marker");

// Exercise both drain policies deterministically without another vendor API
// lifecycle. This fake implements only the documented NativeClient contract.
class FakeClient extends CtpClient {
  constructor(dropOldest, claims) {
    const slotSize = 128, headerSize = 108, numSlots = 4;
    const buffer = new ArrayBuffer(slotSize * numSlots);
    const state = { claims: [...claims], released: 0, drain: undefined };
    const fake = {
      _start(fn) { state.drain = fn; }, _buffer() { return buffer; },
      _claim() { return state.claims.shift() ?? 0; },
      _release(n) { state.released += n; }, _readIndex() { return 0; },
      _info() { return { slotSize, numSlots, headerSize, dropOldest }; },
      _dropCount() { return 0; }, close() {},
    };
    super(fake, native.__layoutData(), new Map([[1, "test"]]));
    this.fakeState = state;
    this.fakeBuffer = buffer;
    this.fakeSlotSize = slotSize;
    this.fakeHeaderSize = headerSize;
    this.start();
  }
  write(index, value) {
    const o = index * this.fakeSlotSize;
    const v = new DataView(this.fakeBuffer);
    v.setInt32(o, 1, true); v.setInt32(o + 8, -1, true);
    v.setInt32(o + 16, -2, true); v.setInt32(o + 20, 4, true);
    v.setInt32(o + this.fakeHeaderSize, value, true);
  }
  fire() { this.fakeState.drain(); }
}

const throwing = new FakeClient(false, [2, 0]);
throwing.write(0, 1); throwing.write(1, 2);
let deliveredAfterThrow = 0, surfaced = false;
throwing.on("test", (value) => {
  if (value === 1) throw new Error("handler boom");
  deliveredAfterThrow++;
});
throwing.on("error", () => { surfaced = true; });
throwing.fire();
await new Promise((resolve) => setImmediate(resolve));
ok(deliveredAfterThrow === 1 && throwing.fakeState.released === 2 && surfaced,
  "a throwing lossless-channel handler is surfaced after the batch without wedging release");
throwing.close();

const closeAtBatchEnd = new FakeClient(false, [1, 0]);
closeAtBatchEnd.write(0, 1);
closeAtBatchEnd.on("test", () => closeAtBatchEnd.close());
closeAtBatchEnd.fire();
ok(closeAtBatchEnd.fakeState.released === 0,
  "lossless drain stops before release when a handler closes at batch end");

const closeBeforeNext = new FakeClient(false, [2, 0]);
closeBeforeNext.write(0, 1); closeBeforeNext.write(1, 2);
closeBeforeNext.on("test", () => closeBeforeNext.close());
closeBeforeNext.fire();
ok(closeBeforeNext.fakeState.released === 0,
  "lossless drain stops before reading the next slot after handler close");

const closedOldest = new FakeClient(true, [1, 0]);
closedOldest.write(0, 1); closedOldest.close(); closedOldest.fire();
ok(closedOldest.fakeState.released === 0,
  "keep-newest drain refuses to read after close");

const closeDuringDecode = new FakeClient(true, [1, 0]);
closeDuringDecode.write(0, 1);
const decodeBeforeClose = closeDuringDecode.decodeSlot.bind(closeDuringDecode);
closeDuringDecode.decodeSlot = (slot) => {
  const rec = decodeBeforeClose(slot);
  closeDuringDecode.close();
  return rec;
};
closeDuringDecode.fire();
ok(closeDuringDecode.batch[0] === undefined && closeDuringDecode.fakeState.released === 0,
  "keep-newest drain clears decoded scratch if close occurs during a batch");

// Preserve normal EventEmitter semantics when no `error` listener exists, but
// defer the throw until after the ring has advanced.
const uncaught = new Promise((resolve) => process.once("uncaughtException", resolve));
c.surfaceHandlerError(new Error("deferred handler failure"));
ok(msgOf(await uncaught).includes("deferred handler failure"),
  "a deferred handler failure is rethrown when no error listener exists");

c.close(); // release the native doorbell handle so the process exits
c.close(); // idempotent
await rejects(c.request(() => 0), "client closed", "request rejects after close");
await rejects(c.queryRequest(() => 0), "client closed", "queryRequest rejects after close");
await rejects(c.submit(() => 0), "client closed", "submit rejects after close");

// close() cancels the active query-spacing timer and rejects both the waiting
// query and every not-yet-sent query in the serialized tail immediately.
const c2 = new MarketData("./flow-cu-close/", "tcp://127.0.0.1:1");
c2.lastQuerySentAt = performance.now();
let sendsAfterClose = 0;
const q1 = c2.queryRequest(() => { sendsAfterClose++; return 0; });
const q2 = c2.queryRequest(() => { sendsAfterClose++; return 0; });
await Promise.resolve(); // let q1 install its 1.1s spacing timer
c2.close();
const closeReturned = Date.now();
const closedQueries = await Promise.allSettled([q1, q2]);
ok(closedQueries.every((x) => x.status === "rejected") &&
   sendsAfterClose === 0 && Date.now() - closeReturned < 250,
  "close cancels queued query waits and rejects the unsent queue promptly");

// Calling a saved spacing completion twice is harmless (timer and invalidation
// can race to invoke it).
const c3 = new FakeClient(false, [0]);
c3.lastQuerySentAt = performance.now();
const delayedReject = c3.queryRequest(() => -1);
await Promise.resolve();
const finishDelay = c3.cancelQueryDelay;
finishDelay(); finishDelay();
await rejects(delayedReject, "code -1", "query spacing completion is idempotent");
c3.close();

ok(recreateWarnings.length === 1,
  "invalid lifecycle-warning configuration falls back safely and warning fires only once");
console.warn = realWarn;

console.log(`CLIENT-UNIT TEST: ${pass} pass, ${fail} fail`);
if (fail) process.exit(1);
