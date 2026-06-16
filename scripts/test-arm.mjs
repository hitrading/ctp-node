// Arm auto-fire test WITHOUT network. Proves the full latency path:
// MD depth tick (SPI) -> ArmRegistry.onTick -> Trader.fireArmed (risk gate +
// ReqOrderInsert) -> one-shot. All in C++ on the callback thread; the inject
// drives the real OnRtnDepthMarketData so onTick runs.
import { MarketData, Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-md", { recursive: true });
mkdirSync("flow-td", { recursive: true });

const md = new MarketData("./flow-md/", "tcp://127.0.0.1:1");
const td = new Trader("./flow-td/", "tcp://127.0.0.1:1");
td.riskSet({ maxOrderVolume: 100 });

// Buy rb2510 when ask <= 3501. The injected sample tick has askPrice1 = 3501.
const handle = td.arm(md, {
  instrumentId: "rb2510",
  side: "buy",
  triggerPrice: 3501,
  order: {
    brokerId: "9999",
    investorId: "test",
    instrumentId: "rb2510",
    direction: "0",
    combOffsetFlag: "0", // open (required; a missing flag would misfire as a close)
    limitPrice: 3500,
    volumeTotalOriginal: 1,
  },
});

// onTick -> fireArmed runs synchronously inside the inject (same thread). On the
// dead test front the send is refused, so the fire counts as "blocked"; total
// attempts (fired + blocked) is what proves the trigger fired exactly once.
const attempts = () => td._armFireCount() + td._armBlockedCount();
md._injectTestTick();
const a1 = attempts();
md._injectTestTick(); // one-shot: must NOT fire again
const a2 = attempts();
handle.disarm();

// A SELL trigger fires on bid >= triggerPrice. CTP reports BidPrice1 = DBL_MAX
// when a side is empty (e.g. at limit-down there are no bids), so without a
// sentinel guard the sell would fire on that DBL_MAX - dumping a sell at the
// worst possible moment. Verify the DBL_MAX no-bid is ignored, but a real high
// bid still fires.
const sell = td.arm(md, {
  instrumentId: "rb2510",
  side: "sell",
  triggerPrice: 3000,
  order: {
    brokerId: "9999", investorId: "test", instrumentId: "rb2510",
    direction: "1", combOffsetFlag: "0", limitPrice: 3000, volumeTotalOriginal: 1,
  },
});
const sBefore = attempts();
md._injectTestTick(Number.MAX_VALUE, 9999); // bid = DBL_MAX no-bid sentinel -> must NOT fire
const sSentinel = attempts();
md._injectTestTick(3600, 9999); // bid = 3600 (real, >= 3000) -> MUST fire
const sReal = attempts();
sell.disarm();

const sellSentinelOk = sSentinel === sBefore;
const sellRealOk = sReal === sBefore + 1;
const ok = a1 === 1 && a2 === 1 && sellSentinelOk && sellRealOk;
console.log(
  `ARM TEST: buy attempts=${a1} (one-shot=${a2}); sell ignores DBL_MAX no-bid=${sellSentinelOk}, sell fires on real bid=${sellRealOk} -> ${ok ? "PASS" : "FAIL"}`
);

md.close();
td.close();
process.exitCode = ok ? 0 : 1;
