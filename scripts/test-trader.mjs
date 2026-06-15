// Trader test WITHOUT network. Exercises the full request path end-to-end:
// JS encode (plain camelCase obj -> struct bytes) -> native _req -> generated
// traderReq switch -> CThostFtdcInputOrderField -> pre-trade RiskEngine.
// The risk verdict (which reads the decoded struct fields) proves encode +
// dispatch + risk wiring all agree, with no live front.
import { Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-td", { recursive: true });
const td = new Trader("./flow-td/", "tcp://127.0.0.1:1");
console.log("Trader api version:", td.getApiVersion());

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

td.riskSet({ maxOrderVolume: 5 });

// 1) Oversized order -> risk reads VolumeTotalOriginal (100 > 5) and blocks.
try {
  await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", limitPrice: 3500, volumeTotalOriginal: 100 });
  check(false, "oversized order should have been rejected");
} catch (e) {
  check(/risk/i.test(e.message), `oversized order blocked by risk ("${e.message}")`);
}

// 2) Kill-switch -> any order blocked.
td.halt();
try {
  await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", limitPrice: 3500, volumeTotalOriginal: 1 });
  check(false, "halted order should have been rejected");
} catch (e) {
  check(/risk/i.test(e.message), `kill-switch blocked order ("${e.message}")`);
}
td.resume();

// 3) Small order passes risk -> reaches the CTP API (would reject SYNCHRONOUSLY
//    if risk blocked; pending/CTP-error means it got past risk).
const r = td
  .reqOrderInsert({ instrumentId: "rb2510", direction: "0", limitPrice: 3500, volumeTotalOriginal: 1 })
  .then(() => "resolved")
  .catch((e) => `rejected: ${e.message}`);
const outcome = await Promise.race([r, new Promise((res) => setTimeout(() => res("pending"), 250))]);
check(!/risk|rate limited/i.test(String(outcome)), `small order passed risk, reached API (outcome: ${outcome})`);

console.log(`TRADER TEST: ${pass} pass, ${fail} fail`);
td.close();
process.exitCode = fail ? 1 : 0;
