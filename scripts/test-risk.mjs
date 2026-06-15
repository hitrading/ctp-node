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

console.log(`RISK DEVIATION TEST: ${pass} pass, ${fail} fail`);
td.close();
process.exitCode = fail ? 1 : 0;
