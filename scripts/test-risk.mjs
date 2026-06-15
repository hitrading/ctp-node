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

console.log(`RISK TEST: ${pass} pass, ${fail} fail`);
td.close();
process.exitCode = fail ? 1 : 0;
