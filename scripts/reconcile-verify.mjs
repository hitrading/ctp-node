// Live validation of (1) multi-terminal awareness: a working order placed by
// session B is counted against session A's position cap; and (2) syncOrders():
// rebuilding the in-flight reservation from reqQryOrder after the local state
// was cleared (as a reconnect would). au2608, real working (resting) orders.
import { MarketData, Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";
import process from "node:process";

const env = process.env;
const C = {
  brokerId: env.CTP_BROKER ?? "9999", userId: env.CTP_USER ?? "", password: env.CTP_PASS ?? "",
  appId: env.CTP_APPID ?? "simnow_client_test", authCode: env.CTP_AUTHCODE ?? "0000000000000000",
  md: env.CTP_MD_FRONT, td: env.CTP_TD_FRONT,
};
const I = "au2608";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
["flow-a", "flow-b", "flow-md"].forEach((d) => mkdirSync(d, { recursive: true }));
let pass = 0, fail = 0;
const check = (c, m) => (c ? (pass++, log("  PASS:", m)) : (fail++, log("  ** FAIL:", m)));
const ob = { brokerId: C.brokerId, investorId: C.userId, combHedgeFlag: "1", orderPriceType: "2", timeCondition: "3", volumeCondition: "1", contingentCondition: "1", forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0 };
const rt = (p) => Number((Math.round(p / 0.02) * 0.02).toFixed(2));

const px = {};
const md = new MarketData("./flow-md/", C.md);
md.on("front-connected", async () => { await md.login({ brokerId: C.brokerId, userId: C.userId, password: C.password }).catch(() => {}); md.subscribe([I]); });
md.on("rtn-depth-market-data", (t) => (px[t.instrumentId] = { bid: t.bidPrice1, ask: t.askPrice1, last: t.lastPrice, lower: t.lowerLimitPrice }));
const login = (t) => t.reqAuthenticate({ brokerId: C.brokerId, userId: C.userId, appId: C.appId, authCode: C.authCode }).catch(() => {}).then(() => t.reqUserLogin({ brokerId: C.brokerId, userId: C.userId, password: C.password }));
const restBuy = () => rt(px[I].last * 0.96); // below market, within limits -> rests as a working order
const probePx = () => rt(px[I].lower - 1); // below limit-down: exchange rejects if it passes the gate (no lingering order)
// restOpen: an order we WANT to persist (rests); probeOpen: a gate test (won't linger)
const restOpen = (t, vol) => t.reqOrderInsert({ ...ob, instrumentId: I, direction: "0", combOffsetFlag: "0", limitPrice: restBuy(), volumeTotalOriginal: vol }).then(() => "SENT").catch((e) => e.message);
const probeOpen = (t, vol) => t.reqOrderInsert({ ...ob, instrumentId: I, direction: "0", combOffsetFlag: "0", limitPrice: probePx(), volumeTotalOriginal: vol }).then(() => "SENT").catch((e) => e.message);

async function cancelWorking(t) {
  const orders = await t.reqQryOrder({ brokerId: C.brokerId, investorId: C.userId });
  for (const o of orders.filter((o) => o.orderStatus === "1" || o.orderStatus === "3")) {
    await t.reqOrderAction({ brokerId: C.brokerId, investorId: C.userId, instrumentId: o.instrumentId, exchangeId: o.exchangeId, orderSysId: o.orderSysId, actionFlag: "0" });
    await sleep(800);
  }
}

const A = new Trader("./flow-a/", C.td);
const B = new Trader("./flow-b/", C.td);
let aReady = false;
A.on("front-connected", async () => { await login(A); log("A logged in"); aReady = true; });
B.on("front-connected", async () => {
  try {
    for (let i = 0; i < 30 && !aReady; i++) await sleep(300);
    await login(B); log("B logged in");
    for (let i = 0; i < 30 && !px[I]; i++) await sleep(300);
    await A.syncMultipliers([I]);
    await A.syncPositions();
    A.setMaxPosition(I, { long: 2 }); // A's cap: 2 long lots
    log(`A: setMaxPosition(${I}, {long:2}); start clean`);
    await cancelWorking(A); await cancelWorking(B); await sleep(1000);

    // ---- (1) multi-terminal: B's working order counts against A's cap ----
    log("== multi-terminal ==");
    log(`B places resting open ${I} x1 @ ${restBuy()} -> ${await restOpen(B, 1)}`);
    await sleep(2500); // A receives B's OnRtnOrder -> reserves under B's session key
    check(/position volume/i.test(await probeOpen(A, 2)), "A: open 2 blocked (B's 1 working + 2 > cap 2) -> A counts another terminal's order");
    await cancelWorking(B); // B cancels -> A receives terminal -> releases B's reservation
    await sleep(2500);
    check(/SENT/.test(await probeOpen(A, 2)), "A: open 2 passes after B cancels (B's reservation released)");
    await sleep(2000); // probe was exchange-rejected; let its transient reservation release

    // ---- (2) syncOrders(): rebuild reservation from reqQryOrder ----
    log("== syncOrders rebuild ==");
    const placed = await restOpen(A, 1);
    log(`A places resting open ${I} x1 @ ${restBuy()} -> ${placed}`);
    check(/SENT/.test(placed), "A: resting order placed (clean slate before resync)");
    await sleep(2500);
    A.resetPositions(); // simulate a reconnect wiping local reservation state
    log("A.resetPositions() (simulating reconnect: local reservations cleared)");
    const n = await A.syncOrders(); // rebuild from the broker's working orders
    log(`A.syncOrders() re-reserved ${n} working order(s)`);
    check(n >= 1, `syncOrders found A's working order (n=${n})`);
    check(/position volume/i.test(await probeOpen(A, 2)), "A: open 2 blocked again after syncOrders (reservation rebuilt: 1 + 2 > 2)");

    log(`\nRESULT: ${pass} pass, ${fail} fail`);
  } catch (e) { log("FAILED:", e.message, "errorId=", e.errorId); }
  finally {
    await cancelWorking(A).catch(() => {}); await cancelWorking(B).catch(() => {});
    await sleep(800);
    const posn = await A.reqQryInvestorPosition({ brokerId: C.brokerId, investorId: C.userId }).catch(() => []);
    const ordn = await A.reqQryOrder({ brokerId: C.brokerId, investorId: C.userId }).catch(() => []);
    log(`FINAL: open positions=${posn.filter((p) => p.position > 0).length}, working orders=${ordn.filter((o) => o.orderStatus === "1" || o.orderStatus === "3").length}`);
    md.close(); A.close(); B.close(); process.exit(0);
  }
});

log("connecting...");
setTimeout(() => { try { md.close(); A.close(); B.close(); } catch {} process.exit(1); }, Number(env.CTP_RUN_MS ?? 70000));
