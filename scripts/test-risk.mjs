// maxPriceDeviation test (no network). Sets a reference price and checks that
// orders deviating too far are blocked by the C++ risk gate, in-range orders
// pass through to the API, and instruments without a reference skip the check.
import { Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-td", { recursive: true });
const td = new Trader("./flow-td/", "tcp://127.0.0.1:1");
td.riskSet({ maxPriceDeviation: 0.05 }); // 5%
td.setRefPrice("rb2610", 3500);

let pass = 0;
let fail = 0;
const check = (c, m) => (c ? (pass++, console.log("  PASS:", m)) : (fail++, console.error("  FAIL:", m)));
const place = (instrumentId, limitPrice) =>
  td.reqOrderInsert({ instrumentId, direction: "0", limitPrice, volumeTotalOriginal: 1 })
    .then(() => "accepted")
    .catch((e) => e.message);

const inRange = await place("rb2610", 3600); // 2.86% of 3500
check(!/risk/i.test(String(inRange)), `in-range (3600 vs 3500) passed deviation -> ${inRange}`);

const outOfRange = await place("rb2610", 5000); // 42.8%
check(/risk/i.test(String(outOfRange)), `out-of-range (5000 vs 3500) blocked -> ${outOfRange}`);

const noRef = await place("ag2512", 99999); // no reference set
check(!/risk/i.test(String(noRef)), `no reference -> deviation skipped -> ${noRef}`);

// ----- max position cost (open-cost sum) -----
td.setMultiplier("rb2610", 10); // contract multiplier
td.riskSet({ maxPositionCost: 100000 }); // 100k cap (resets other limits)
td._applyTestTrade("rb2610", true, true, 3000, 2); // open BUY 2 @ 3000 -> 60000
check(Math.abs(td.positionCost() - 60000) < 1, `position cost after open = ${td.positionCost()} (expect 60000)`);

const openOrder = (vol) =>
  td.reqOrderInsert({ instrumentId: "rb2610", direction: "0", combOffsetFlag: "0", limitPrice: 3000, volumeTotalOriginal: vol })
    .then(() => "sent")
    .catch((e) => e.message);

const within = await openOrder(1); // +30000 -> 90000 <= 100000
check(!/position/i.test(String(within)), `open within cap passed -> ${within}`);
const over = await openOrder(2); // +60000 -> 120000 > 100000
check(/position/i.test(String(over)), `open over cap blocked -> ${over}`);

const closing = await td
  .reqOrderInsert({ instrumentId: "rb2610", direction: "1", combOffsetFlag: "1", limitPrice: 3000, volumeTotalOriginal: 5 })
  .then(() => "sent")
  .catch((e) => e.message);
check(!/position/i.test(String(closing)), `closing order not capped -> ${closing}`);

td._applyTestTrade("rb2610", false, false, 3100, 1); // close 1 of the long 2 -> release half
check(Math.abs(td.positionCost() - 30000) < 1, `position cost after partial close = ${td.positionCost()} (expect 30000)`);

// ----- regression: NaN/Inf must never poison position cost -----
// onTrade/seedPosition reject non-finite/negative; setMultiplier must reject
// +Inf too (it passes the ">0" test, and an Inf multiplier makes cost Inf and a
// proportional close-release NaN, which silently voids the cap since NaN>cap is
// false). All three must leave positionCost finite.
td.resetPositions();
td.setMultiplier("rb2610", 10);
td._applyTestTrade("rb2610", true, true, NaN, 1);
td._applyTestTrade("rb2610", true, true, Infinity, 1);
td._applyTestTrade("rb2610", true, true, 3000, NaN);
td._applyTestTrade("rb2610", true, true, -3000, 1);
check(td.positionCost() === 0, `onTrade NaN/Inf/neg ignored -> cost 0 (got ${td.positionCost()})`);
td.seedPosition("rb2610", "long", Infinity, 1000);
td.seedPosition("rb2610", "long", 5, NaN);
check(td.positionCost() === 0, `seedPosition NaN/Inf ignored -> cost 0 (got ${td.positionCost()})`);
td.resetPositions();
td.setMultiplier("inf2610", Infinity); // must fall back to 1.0, not store Inf
td._applyTestTrade("inf2610", true, true, 100, 2);
check(Number.isFinite(td.positionCost()), `setMultiplier(Inf) -> finite cost (got ${td.positionCost()})`);
td._applyTestTrade("inf2610", false, false, 100, 1); // proportional release must not be NaN
check(Number.isFinite(td.positionCost()), `partial close after Inf-mult guard -> finite cost (got ${td.positionCost()})`);
td.resetPositions();
td.setMultiplier("rb2610", 10); // restore for downstream checks

// ----- regression: resetPositions() must NOT wipe multipliers -----
// syncPositions() calls resetPositions() before re-seeding; if the multiplier
// lived in the same map it would be lost, and the next fill would be costed at
// x1 (the live bug: positionCost 3180 instead of 31800).
td.resetPositions();
check(Math.abs(td.positionCost()) < 1, `positionCost cleared by reset -> ${td.positionCost()}`);
td._applyTestTrade("rb2610", true, true, 3000, 1); // open BUY 1 @ 3000 after a reset
check(Math.abs(td.positionCost() - 30000) < 1, `multiplier survived resetPositions: cost = ${td.positionCost()} (expect 30000 = 3000*1*10, NOT 3000)`);

// ----- per-instrument max position volume (lots) -----
td.resetPositions();
td.riskSet({}); // clear scalar caps so only the per-instrument lot cap is active
td.setMaxPosition("rb2610", 3); // max 3 lots per side
const vOpen = (dir, vol) =>
  td.reqOrderInsert({ instrumentId: "rb2610", direction: dir, combOffsetFlag: "0", limitPrice: 3000, volumeTotalOriginal: vol })
    .then(() => "sent")
    .catch((e) => e.message);

check(!/volume/i.test(String(await vOpen("0", 3))), `open 3 within max-position (0+3<=3) passed`);
td._applyTestTrade("rb2610", true, true, 3000, 2); // fill: now hold 2 long
check(/volume/i.test(String(await vOpen("0", 2))), `open 2 over max-position (2+2>3) blocked`);
check(!/volume/i.test(String(await vOpen("0", 1))), `open 1 within max-position (2+1<=3) passed`);
check(!/volume/i.test(String(await vOpen("1", 3))), `short-open 3 independent of long side passed`);

// asymmetric caps: long 5, short 2 (separate instrument, starts flat)
td.setMaxPosition("ru2610", { long: 5, short: 2 });
const ruOpen = (dir, vol) =>
  td.reqOrderInsert({ instrumentId: "ru2610", direction: dir, combOffsetFlag: "0", limitPrice: 10000, volumeTotalOriginal: vol })
    .then(() => "sent")
    .catch((e) => e.message);
check(!/volume/i.test(String(await ruOpen("0", 5))), `asym long 5 within long-cap(5) passed`);
check(/volume/i.test(String(await ruOpen("0", 6))), `asym long 6 over long-cap(5) blocked`);
check(/volume/i.test(String(await ruOpen("1", 3))), `asym short 3 over short-cap(2) blocked`);
check(!/volume/i.test(String(await ruOpen("1", 2))), `asym short 2 within short-cap(2) passed`);

// ----- per-instrument max position COST (gross capital per contract) -----
td.resetPositions();
td.riskSet({}); // clear the global cost cap; only the per-instrument caps active
td.setMultiplier("au2608", 1000);
td.setMultiplier("ag2608", 15);
td.setMaxPositionCost("au2608", 1_000_000); // 1M per contract
td.setMaxPositionCost("ag2608", 1_000_000); // same numeric cap, cheaper per lot
const cOpen = (id, px, vol) =>
  td.reqOrderInsert({ instrumentId: id, direction: "0", combOffsetFlag: "0", limitPrice: px, volumeTotalOriginal: vol })
    .then(() => "sent")
    .catch((e) => e.message);
check(!/position cost/i.test(String(await cOpen("au2608", 560, 1))), `au 1 lot (560k) within 1M passed`);
check(/position cost/i.test(String(await cOpen("au2608", 560, 2))), `au 2 lots (1.12M) over 1M blocked`);
td._applyTestTrade("au2608", true, true, 560, 1); // fill: hold 560k of au
check(/position cost/i.test(String(await cOpen("au2608", 560, 1))), `au +1 lot with 560k held (1.12M) blocked`);
check(!/position cost/i.test(String(await cOpen("ag2608", 7500, 1))), `ag 1 lot (112.5k) independent of au's holding passed`);

// ----- in-flight reservation (burst protection) -----
// committed = held + working(in-flight) open orders, so a burst of opens can't
// slip past the cap before fills report. (_applyTestOrder simulates OnRtnOrder.)
td.resetPositions();
td.riskSet({});
td.setMaxPosition("rb2610", 5); // 5 lots/side
const vOpen2 = (vol) =>
  td.reqOrderInsert({ instrumentId: "rb2610", direction: "0", combOffsetFlag: "0", limitPrice: 3000, volumeTotalOriginal: vol })
    .then(() => "sent")
    .catch((e) => e.message);
td._applyTestOrder(1, 100, "w1", "rb2610", true, true, "3", 3000, 4, 0); // a working open of 4 -> reserves 4
check(/volume/i.test(String(await vOpen2(2))), `in-flight 4 + open 2 (6>5) blocked by reservation`);
check(!/volume/i.test(String(await vOpen2(1))), `in-flight 4 + open 1 (5<=5) passed`);
td._applyTestOrder(1, 100, "w1", "rb2610", true, true, "0", 3000, 4, 4); // fully filled -> reservation released
td._applyTestTrade("rb2610", true, true, 3000, 4); // the fill becomes held
check(!/volume/i.test(String(await vOpen2(1))), `after fill: held 4 + open 1 (5<=5) passed`);
check(/volume/i.test(String(await vOpen2(2))), `after fill: held 4 + open 2 (6>5) blocked`);
td.resetPositions();
td.setMaxPosition("rb2610", 5);
td._applyTestOrder(1, 100, "w2", "rb2610", true, true, "3", 3000, 5, 0); // reserve 5 = the whole cap
check(/volume/i.test(String(await vOpen2(1))), `in-flight 5 (=cap) -> open 1 blocked`);
td._applyTestOrder(1, 100, "w2", "rb2610", true, true, "5", 3000, 5, 0); // cancelled -> released
check(!/volume/i.test(String(await vOpen2(1))), `after cancel: open 1 passed (reservation freed)`);

// another-terminal simulation: a working order from a DIFFERENT session is
// tracked independently (keyed by front:session:ref, no collision with ours)
td.resetPositions();
td.setMaxPosition("rb2610", 3);
td._applyTestOrder(1, 100, "1", "rb2610", true, true, "3", 3000, 2, 0); // "their" ref 1 (sess 100)
td._applyTestOrder(9, 999, "1", "rb2610", true, true, "3", 3000, 2, 0); // "our-ish" ref 1 (sess 999) - same ref number!
check(/volume/i.test(String(await vOpen2(1))), `two sessions' ref "1" both counted (2+2+1>3) -> blocked (no key collision)`);
td._applyTestOrder(1, 100, "1", "rb2610", true, true, "5", 3000, 2, 0); // their order cancelled -> release only theirs
check(!/volume/i.test(String(await vOpen2(1))), `after one session's cancel: 2 left, +1<=3 passed`);

// ----- DBL_MAX (CTP unset-price sentinel) must not poison the reference price -----
// CTP fills unset price fields with DBL_MAX before the first trade prints; a user
// wiring tick.lastPrice -> setRefPrice would otherwise set ref=DBL_MAX, making
// |p-ref|/ref == 1.0 and blocking EVERY deviation-checked order. setRefPrice must
// ignore the sentinel so the reference simply stays unset.
td.resetPositions();
td.riskSet({ maxPriceDeviation: 0.05 });
td.setRefPrice("au2608", Number.MAX_VALUE); // DBL_MAX sentinel -> must be ignored
const dblmax = await td
  .reqOrderInsert({ instrumentId: "au2608", direction: "0", combOffsetFlag: "0", limitPrice: 560, volumeTotalOriginal: 1 })
  .then(() => "sent").catch((e) => e.message);
check(!/deviat/i.test(String(dblmax)), `DBL_MAX ref ignored -> normal order not falsely blocked -> ${dblmax}`);
td.setRefPrice("au2608", Infinity); // non-finite -> ignored too
td.setRefPrice("au2608", 560); // a real print activates the check
const farOff = await td
  .reqOrderInsert({ instrumentId: "au2608", direction: "0", combOffsetFlag: "0", limitPrice: 9999, volumeTotalOriginal: 1 })
  .then(() => "sent").catch((e) => e.message);
check(/deviat/i.test(String(farOff)), `after a real ref (560), a far price (9999) is blocked -> ${farOff}`);

// ----- transient OrderStatus holds the reservation (only terminal states release) -----
// A conditional order can report Unknown 'a' / NotTouched 'b' / Touched 'c'
// before it reaches NoTradeQueueing '3'; releasing the reservation on those
// would briefly drop the cap. Only 0/2/4/5 (done) release.
td.resetPositions();
td.riskSet({});
td.setMaxPosition("rb2610", 3);
td._applyTestOrder(1, 100, "t1", "rb2610", true, true, "a", 3000, 3, 0); // Unknown, working 3
check(/volume/i.test(String(await vOpen2(1))), `transient 'a' (Unknown) still reserves -> 3+1>3 blocked`);
td._applyTestOrder(1, 100, "t1", "rb2610", true, true, "b", 3000, 3, 0); // NotTouched, still working
check(/volume/i.test(String(await vOpen2(1))), `transient 'b' (NotTouched) still reserves -> blocked`);
td._applyTestOrder(1, 100, "t1", "rb2610", true, true, "3", 3000, 3, 0); // now queueing, unchanged
check(/volume/i.test(String(await vOpen2(1))), `'3' (Queueing) after transients keeps the same reservation -> blocked`);
td._applyTestOrder(1, 100, "t1", "rb2610", true, true, "5", 3000, 3, 0); // cancelled -> release
check(!/volume/i.test(String(await vOpen2(1))), `terminal '5' (Canceled) releases the reservation -> +1<=3 passed`);

// ----- kill-switch (halt) covers ALL position-opening inserts, not just orders -----
// halt() must block regular/exec/quote/forquote/option-self-close/comb inserts;
// cancels & other actions stay open so you can pull working orders while halted.
td.resetPositions();
td.riskSet({});
const callRaw = (fn, arg) => td[fn](arg).then(() => "sent").catch((e) => e.message);
td.halt();
check(/risk/i.test(String(await callRaw("reqOrderInsert", { instrumentId: "au2608", direction: "0", limitPrice: 560, volumeTotalOriginal: 1 }))), `halt blocks reqOrderInsert`);
check(/risk/i.test(String(await callRaw("reqExecOrderInsert", { instrumentId: "au2608", volume: 1 }))), `halt blocks reqExecOrderInsert`);
check(/risk/i.test(String(await callRaw("reqQuoteInsert", { instrumentId: "au2608" }))), `halt blocks reqQuoteInsert`);
check(/risk/i.test(String(await callRaw("reqForQuoteInsert", { instrumentId: "au2608" }))), `halt blocks reqForQuoteInsert`);
check(/risk/i.test(String(await callRaw("reqOptionSelfCloseInsert", { instrumentId: "au2608", volume: 1 }))), `halt blocks reqOptionSelfCloseInsert`);
check(/risk/i.test(String(await callRaw("reqCombActionInsert", { instrumentId: "au2608" }))), `halt blocks reqCombActionInsert`);
check(!/risk/i.test(String(await callRaw("reqOrderAction", { instrumentId: "au2608" }))), `halt does NOT block reqOrderAction (cancels stay open)`);
check(!/risk/i.test(String(await callRaw("reqExecOrderAction", { instrumentId: "au2608" }))), `halt does NOT block reqExecOrderAction`);
td.resume();
check(!/risk/i.test(String(await callRaw("reqExecOrderInsert", { instrumentId: "au2608", volume: 1 }))), `after resume reqExecOrderInsert no longer blocked`);

console.log(`RISK TEST: ${pass} pass, ${fail} fail`);
td.close();
process.exitCode = fail ? 1 : 0;
