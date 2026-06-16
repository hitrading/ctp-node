// Coverage for client.ts request/response machinery that a live connection would
// normally drive. TS access modifiers don't exist at runtime, so we drive the
// real methods (request/submit/deliver/armRequestTimer) directly with stub send
// callbacks and crafted decoded records — deterministic, no network. A MarketData
// is the vehicle (its zeroIdFallbackEvents includes the login event).
import { MarketData } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-cu", { recursive: true });
let pass = 0, fail = 0;
const ok = (c, m) => { c ? (pass++, console.log("  PASS:", m)) : (fail++, console.log("  FAIL:", m)); };
const msgOf = (e) => String((e && e.message) || e);
const rejects = async (p, frag, m) => {
  try { await p; ok(false, m + " (did not reject)"); }
  catch (e) { ok(!frag || msgOf(e).includes(frag), m + (frag && !msgOf(e).includes(frag) ? ` (got: ${msgOf(e)})` : "")); }
};
const resolves = async (p, m) => { try { await p; ok(true, m); } catch (e) { ok(false, m + ` (rejected: ${msgOf(e)})`); } };

const c = new MarketData("./flow-cu/", "tcp://127.0.0.1:1");

// --- request(): non-zero send codes route through riskRejectMessage; throws/timeout ---
await rejects(c.request(() => -10001), "pre-trade risk", "request -10001 -> pre-trade risk");
await rejects(c.request(() => -10002), "rate limited", "request -10002 -> rate limited");
await rejects(c.request(() => -10003), "max position cost", "request -10003 -> position cost");
await rejects(c.request(() => -10004), "max position volume", "request -10004 -> position volume");
await rejects(c.request(() => -1), "code -1", "request generic non-zero code");
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

let r2; const id2 = c.nextRequestId(); c.pending.set(id2, mk((v) => { r2 = v; }, () => {}, false));
c.deliver({ evt: 0x1006, value: { n: 1 }, options: { requestId: id2, isLast: false } });
c.deliver({ evt: 0x1006, value: { n: 2 }, options: { requestId: id2, isLast: true } });
ok(Array.isArray(r2) && r2.length === 2, "deliver accumulates multi-row, then resolves on isLast");

let e3; const id3 = c.nextRequestId(); c.pending.set(id3, mk(() => {}, (er) => { e3 = er; }, true));
c.deliver({ evt: 0x1007, value: undefined, options: { requestId: id3, isLast: true, rspInfo: { errorId: 5, errorMsg: "bad" } } });
ok(e3 && e3.errorId === 5, "deliver rejects on an rspInfo error response");

let r4; const id4 = c.nextRequestId(); c.pending.set(id4, mk((v) => { r4 = v; }, () => {}, true));
c.deliver({ evt: 0x1004, value: { z: 9 }, options: { requestId: undefined, isLast: true } });
ok(r4 && r4.z === 9, "deliver zero-id fallback resolves the oldest pending (MD login)");

let emitted = false; c.on("rtn-depth-market-data", () => { emitted = true; });
c.deliver({ evt: 0x100c, value: { t: 1 }, options: { requestId: undefined, isLast: undefined } });
ok(emitted, "deliver emits a streaming push without correlation");

let unknown = false; c.on("event:39321", () => { unknown = true; });
c.deliver({ evt: 0x9999, value: undefined, options: { requestId: undefined, isLast: undefined } });
ok(unknown, "deliver emits event:<id> for an unmapped event code");

// --- idle timeout + the timeout<=0 no-op ---
c.requestTimeoutMs = 10;
await rejects(c.request(() => 0), "timed out", "request idle timeout rejects");
c.requestTimeoutMs = 0; c.armRequestTimer(987654, mk(() => {}, () => {}, true)); ok(true, "armRequestTimer is a no-op when timeout<=0");
c.requestTimeoutMs = 30000;

// --- rejectAllPending via front-disconnected ---
let dj; const idd = c.nextRequestId();
c.pending.set(idd, { ...mk(() => {}, (er) => { dj = er; }, true), timer: setTimeout(() => {}, 1000) });
c.emit("front-disconnected", 0, {});
ok(dj && /disconnect/.test(msgOf(dj)), "front-disconnected rejects all pending requests");

// --- nextRequestId wraps past int32 max (so it keeps matching the echoed wire id) ---
c.reqSeq = 0x7fffffff;
ok(c.nextRequestId() === 1, "nextRequestId wraps to 1 past int32 max");

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
let dval; c.on("front-disconnected", (v) => { dval = v; });
writeSlot(3, { evt: 0x1002, sid: -2, plen: 4, i32: 7 });
c.dispatch(3 * ss);
ok(dval === 7, "dispatch decodes a slot and delivers it");

c.close(); // release the native doorbell handle so the process exits
console.log(`CLIENT-UNIT TEST: ${pass} pass, ${fail} fail`);
if (fail) process.exit(1);
