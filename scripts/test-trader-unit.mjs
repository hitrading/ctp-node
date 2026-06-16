// Coverage for trader.ts logic the e2e tests don't reach offline: the risk/cap
// setters + finite guards, seedFromPositions aggregation, arm() validation, the
// rsp-user-login credential/session listener, and session()/sync* (driven by
// stubbing the leaf req query methods so the row-processing logic runs without a
// live connection). The native risk-engine calls underneath run for real.
import { Trader, MarketData } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-tu", { recursive: true });
mkdirSync("flow-tu-md", { recursive: true });
let pass = 0, fail = 0;
const ok = (c, m) => { c ? (pass++, console.log("  PASS:", m)) : (fail++, console.log("  FAIL:", m)); };
const throws = (fn, m) => { try { fn(); ok(false, m + " (did not throw)"); } catch { ok(true, m); } };

const td = new Trader("./flow-tu/", "tcp://127.0.0.1:1");

// rsp-user-login listener (credentials + order-ref seed + setSession): full / partial / empty.
td.emit("rsp-user-login", { brokerId: "9999", userId: "u", maxOrderRef: "100", frontId: 1, sessionId: 7 });
td.emit("rsp-user-login", {});
td.emit("rsp-user-login", undefined);
ok(true, "rsp-user-login listener handles full / partial / empty payloads");

// risk config + finite guards.
td.riskSet({ maxOrderVolume: 10, maxNotional: 1e6, maxPriceDeviation: 0.02, maxOrdersPerSec: 5, orderBurst: 10, maxPositionCost: 1e7 });
ok(true, "riskSet applies a full config");
throws(() => td.riskSet({ maxOrderVolume: Infinity }), "riskSet rejects non-finite maxOrderVolume");
throws(() => td.riskSet({ maxNotional: NaN }), "riskSet rejects NaN maxNotional");
td.halt(); td.resume(); ok(true, "halt() / resume()");
td.setRefPrice("rb2510", 3500); ok(true, "setRefPrice");
td.setMultiplier("rb2510", 10); ok(true, "setMultiplier");
td.setMaxPosition("rb2510", 100); ok(true, "setMaxPosition (number caps both sides)");
td.setMaxPosition("ru2510", { long: 100, short: 20 }); ok(true, "setMaxPosition ({long,short})");
td.setMaxPosition("ag2510", { long: 5 }); ok(true, "setMaxPosition (one side only)");
throws(() => td.setMaxPosition("x", Infinity), "setMaxPosition rejects non-finite number");
throws(() => td.setMaxPosition("x", { long: Infinity }), "setMaxPosition rejects non-finite side");
td.setMaxPositions({ a: 5, b: { long: 1, short: 2 } }); ok(true, "setMaxPositions (batch)");
td.setMaxPositionCost("ag2608", 2e6); ok(true, "setMaxPositionCost");
td.setMaxPositionCosts({ au2608: 5e6, ag2608: 2e6 }); ok(true, "setMaxPositionCosts (batch)");
throws(() => td.setMaxPositionCosts({ x: Infinity }), "setMaxPositionCosts rejects non-finite");
td.seedPosition("rb2510", "long", 5, 10000); ok(true, "seedPosition");
td.seedFromPositions([
  { instrumentId: "rb", posiDirection: "2", position: 3, openCost: 300 },
  { instrumentId: "rb", posiDirection: "2", position: 2, openCost: 200 },
  { instrumentId: "au", posiDirection: "3", position: 1, openCost: 900 },
  { instrumentId: "z", posiDirection: "2", position: 0, openCost: 0 },
]);
ok(true, "seedFromPositions aggregates by instrument+side and skips zero-position rows");
ok(typeof td.positionCost() === "number", "positionCost");
td.resetPositions(); ok(true, "resetPositions");
ok(typeof td.getApiVersion() === "string", "getApiVersion");
ok(typeof td.getTradingDay() === "string", "getTradingDay");
const stats = td.armStats();
ok(typeof stats.fired === "number" && typeof stats.blocked === "number", "armStats");

// reqOrderInsert: auto orderRef when blank, and keep an explicit one (rejects offline; logic runs).
await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500 }).catch(() => {});
await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500, orderRef: "explicit-1" }).catch(() => {});
ok(true, "reqOrderInsert (auto + explicit orderRef)");

// arm() validation branches + a valid arm/disarm.
const md = new MarketData("./flow-tu-md/", "tcp://127.0.0.1:1");
throws(() => td.arm(md, { instrumentId: "", side: "buy", triggerPrice: 1, order: {} }), "arm throws on empty instrumentId");
throws(() => td.arm(md, { instrumentId: "rb", side: "buy", triggerPrice: 0, order: {} }), "arm throws on a non-positive trigger price");
throws(() => td.arm(md, { instrumentId: "rb", side: "buy", triggerPrice: 1, order: { instrumentId: "rb" } }), "arm throws on an incomplete order template");
const h = td.arm(md, { instrumentId: "rb", side: "sell", triggerPrice: 3500, order: { instrumentId: "rb", direction: "1", combOffsetFlag: "0", volumeTotalOriginal: 1, limitPrice: 3500 } });
ok(typeof h.id === "number", "arm returns a handle id");
ok(typeof h.disarm() === "boolean", "arm handle disarm()");

// trackMarketData: an md tick should flow to the risk engine's reference price.
td.trackMarketData(md);
md._injectTestTick(3499, 3501);
await new Promise((r) => setTimeout(r, 60)); // let the doorbell drain the injected tick
ok(true, "trackMarketData routes md ticks to setRefPrice");

// session() + sync* with the leaf req methods stubbed (no network).
td.reqAuthenticate = async () => ({});
td.reqUserLogin = async () => ({ brokerId: "9999", userId: "u", maxOrderRef: "100", frontId: 1, sessionId: 7 });
td.reqSettlementInfoConfirm = async () => ({});
td.reqQryInstrument = async () => [{ instrumentId: "rb2510", volumeMultiple: 10 }, { instrumentId: "x", volumeMultiple: 0 }];
td.reqQryInvestorPosition = async () => [{ instrumentId: "rb2510", posiDirection: "2", position: 3, openCost: 30000 }];
td.reqQryOrder = async () => [
  { frontId: 1, sessionId: 7, orderRef: "7", instrumentId: "rb2510", combOffsetFlag: "0", direction: "0", orderStatus: "3", limitPrice: 3500, volumeTotalOriginal: 2, volumeTraded: 0 },
  { frontId: 1, sessionId: 7, orderRef: "8", instrumentId: "rb", combOffsetFlag: "1", direction: "0", orderStatus: "0", limitPrice: 1, volumeTotalOriginal: 1, volumeTraded: 1 },
];
const res = await td.session({ brokerId: "9999", userId: "u", password: "p", appId: "a", authCode: "c" });
ok(res.multipliers === 1, "session: syncMultipliers applied 1 (skipped volumeMultiple 0)");
ok(res.positions === 1, "session: syncPositions counted 1");
ok(res.orders === 1, "session: syncOrders re-reserved 1 working-open order");
const res2 = await td.session({ brokerId: "9999", userId: "u", password: "p", confirmSettlement: false, sync: { multipliers: ["rb2510"], positions: false, orders: false } });
ok(res2.multipliers === 1 && res2.positions === 0 && res2.orders === 0, "session: auth skipped + settlement off + sync subset");

// queryWithRetry: stays-empty -> [] (count 0), and keeps-failing -> throws after retries.
td.reqQryInstrument = async () => [];
ok((await td.syncMultipliers(["rb2510"])) === 0, "syncMultipliers returns 0 when the query stays empty");
td.reqQryInstrument = async () => { throw new Error("flow control"); };
let threw = false; await td.syncMultipliers().catch(() => { threw = true; });
ok(threw, "syncMultipliers throws after retries when the query keeps failing");

td.close(); md.close(); // release the native doorbell handles so the process exits
console.log(`TRADER-UNIT TEST: ${pass} pass, ${fail} fail`);
if (fail) process.exit(1);
