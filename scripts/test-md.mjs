// End-to-end MarketData data-plane test WITHOUT network: inject a synthetic
// depth tick natively (as if from OnRtnDepthMarketData), then verify it flows
// ring -> coalesced doorbell -> JS drain -> decode -> emit as a plain object.
import { MarketData } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-test", { recursive: true });

const md = new MarketData("./flow-test/", "tcp://127.0.0.1:1");
console.log("MarketData api version:", md.getApiVersion());

let got = null;
md.on("rtn-depth-market-data", (tick) => {
  got = tick;
});

md._injectTestTick();

setTimeout(() => {
  const ok =
    got &&
    got.instrumentId === "rb2510" &&
    got.exchangeId === "SHFE" &&
    got.lastPrice === 3500.5 &&
    got.bidPrice1 === 3499 &&
    got.volume === 12345;
  if (ok) {
    console.log(
      "MD INJECT TEST: PASS",
      JSON.stringify({
        instrumentId: got.instrumentId,
        exchangeId: got.exchangeId,
        lastPrice: got.lastPrice,
        bidPrice1: got.bidPrice1,
        bidVolume1: got.bidVolume1,
        volume: got.volume,
      })
    );
  } else {
    console.error("MD INJECT TEST: FAIL", got);
    process.exitCode = 1;
  }
  md.close();
}, 300);
