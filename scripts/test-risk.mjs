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

console.log(`RISK TEST: ${pass} pass, ${fail} fail`);
td.close();
process.exitCode = fail ? 1 : 0;
