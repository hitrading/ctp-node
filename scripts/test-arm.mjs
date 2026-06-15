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
    limitPrice: 3500,
    volumeTotalOriginal: 1,
    orderRef: "arm-1",
  },
});

// onTick -> fireArmed runs synchronously inside the inject (same thread).
md._injectTestTick();
const fired1 = td._armFireCount();
md._injectTestTick(); // one-shot: must NOT fire again
const fired2 = td._armFireCount();

const ok = fired1 === 1 && fired2 === 1;
console.log(
  `ARM TEST: matching tick fired=${fired1}, second tick (one-shot)=${fired2} -> ${ok ? "PASS" : "FAIL"}`
);

handle.disarm();
md.close();
td.close();
process.exitCode = ok ? 0 : 1;
