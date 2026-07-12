// Arm auto-fire test WITHOUT network. Proves the full latency path:
// MD depth tick (SPI) -> ArmRegistry.onTick -> Trader.fireArmed (risk gate +
// ReqOrderInsert) -> one-shot. All in C++ on the callback thread; the inject
// drives the real OnRtnDepthMarketData so onTick runs.
import { MarketData, Trader } from "../dist/index.js";
process.env.CTP_NODE_TEST_HOOKS = "1";
import { mkdirSync } from "node:fs";

mkdirSync("flow-md", { recursive: true });
mkdirSync("flow-td", { recursive: true });

const md = new MarketData("./flow-md/", "tcp://127.0.0.1:1");
const td = new Trader("./flow-td/", "tcp://127.0.0.1:1");
// Offline: seed only the JS order identity through the post-correlation hook.
// Native session/position/order state remains unavailable, which is why hit
// attempts are blocked and retried.
td.onRequestResolved("rsp-user-login", {
  brokerId: "9999", userId: "test", frontId: 1, sessionId: 1, maxOrderRef: "0",
}, {});
td.setRiskLimits({ maxOrderVolume: 100 });

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
const preLoginBefore = attempts();
md._injectTestTick();
const preLoginAfter = attempts();
md._injectTestLifecycle("login");
md._injectTestTick();
const a1 = attempts();
md._injectTestTick(); // backoff: must not spin on every tick
const a2 = attempts();
await new Promise((resolve) => setTimeout(resolve, 120));
md._injectTestTick(); // refused send remains armed and retries after backoff
const a3 = attempts();
handle.disarm();

// A trigger hit during halt must remain armed. After resume and backoff it is
// attempted again (the dead front then refuses the actual send).
const halted = td.arm(md, {
  instrumentId: "rb2510", side: "buy", triggerPrice: 3501,
  order: { instrumentId: "rb2510", direction: "0", combOffsetFlag: "0",
    limitPrice: 3500, volumeTotalOriginal: 1 },
});
const hBefore = attempts();
td.reduceOnly();
md._injectTestTick();
const hDuring = attempts();
td.resume();
await new Promise((resolve) => setTimeout(resolve, 120));
md._injectTestTick();
const hAfter = attempts();
halted.disarm();

// HALT_ALL is the hard-stop transition: it blocks the final send path and
// removes historical mandates, so resume cannot unexpectedly fire an old arm.
const hardStopped = td.arm(md, {
  instrumentId: "rb2510", side: "buy", triggerPrice: 3501,
  order: { instrumentId: "rb2510", direction: "0", combOffsetFlag: "0",
    limitPrice: 3500, volumeTotalOriginal: 1 },
});
const hardBefore = attempts();
td.haltAll();
td.resume();
await new Promise((resolve) => setTimeout(resolve, 120));
md._injectTestTick();
const hardAfter = attempts();
const hardStopOk = hardAfter === hardBefore && hardStopped.disarm() === false;

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

// An armed order must use only ticks accepted by an authenticated MD epoch.
// Racing callbacks before login, after disconnect, or after logout still reach
// JS as protocol events, but must not reach ArmRegistry::onTick.
const disconnectedArm = td.arm(md, {
  instrumentId: "rb2510", side: "buy", triggerPrice: 3501,
  order: { instrumentId: "rb2510", direction: "0", combOffsetFlag: "0",
    limitPrice: 3500, volumeTotalOriginal: 1 },
});
const disconnectedBefore = attempts();
md._injectTestLifecycle("disconnected");
md._injectTestTick();
const disconnectedInvalid = attempts();
md._injectTestLifecycle("login");
md._injectTestTick();
const disconnectedRelogin = attempts();
disconnectedArm.disarm();

const loggedOutArm = td.arm(md, {
  instrumentId: "rb2510", side: "buy", triggerPrice: 3501,
  order: { instrumentId: "rb2510", direction: "0", combOffsetFlag: "0",
    limitPrice: 3500, volumeTotalOriginal: 1 },
});
const logoutBefore = attempts();
md._injectTestLifecycle("logout");
md._injectTestTick();
const logoutInvalid = attempts();
md._injectTestLifecycle("login");
md._injectTestTick();
const logoutRelogin = attempts();
loggedOutArm.disarm();

const sellSentinelOk = sSentinel === sBefore;
const sellRealOk = sReal === sBefore + 1;
const haltRetryOk = hDuring === hBefore + 1 && hAfter === hBefore + 2;
const mdEpochOk = preLoginAfter === preLoginBefore &&
  disconnectedInvalid === disconnectedBefore && disconnectedRelogin === disconnectedBefore + 1 &&
  logoutInvalid === logoutBefore && logoutRelogin === logoutBefore + 1;
const ok = a1 === 1 && a2 === 1 && a3 === 2 && haltRetryOk && hardStopOk &&
  sellSentinelOk && sellRealOk && mdEpochOk;
console.log(
  `ARM TEST: refused buy stays armed/backoff=${a1},${a2},${a3}; halt retry=${haltRetryOk}; haltAll clears=${hardStopOk}; sell ignores DBL_MAX no-bid=${sellSentinelOk}, sell fires on real bid=${sellRealOk}; MD session gates arms=${mdEpochOk} -> ${ok ? "PASS" : "FAIL"}`
);

md.close();
td.close();
process.exitCode = ok ? 0 : 1;
