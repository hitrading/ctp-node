// Probe: does a session receive OnRtnOrder/OnRtnTrade for orders placed by a
// DIFFERENT session of the same investor (i.e. another terminal)? Session A
// only observes; session B places+closes one au2608 lot. We log whether A sees
// B's order/trade returns (and whose FrontID/SessionID they carry).
import { MarketData, Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";
import process from "node:process";

const env = process.env;
const C = {
  brokerId: env.CTP_BROKER ?? "9999", userId: env.CTP_USER ?? "", password: env.CTP_PASS ?? "",
  appId: env.CTP_APPID ?? "simnow_client_test", authCode: env.CTP_AUTHCODE ?? "0000000000000000",
  md: env.CTP_MD_FRONT, td: env.CTP_TD_FRONT,
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
["flow-a", "flow-b", "flow-md"].forEach((d) => mkdirSync(d, { recursive: true }));
const ob = { brokerId: C.brokerId, investorId: C.userId, combHedgeFlag: "1", orderPriceType: "2", timeCondition: "3", volumeCondition: "1", contingentCondition: "1", forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0 };

const px = {};
const md = new MarketData("./flow-md/", C.md);
md.on("front-connected", async () => { await md.login({ brokerId: C.brokerId, userId: C.userId, password: C.password }).catch(() => {}); md.subscribe(["au2608"]); });
md.on("rtn-depth-market-data", (t) => (px[t.instrumentId] = { bid: t.bidPrice1, ask: t.askPrice1 }));

const login = (t) => t.reqAuthenticate({ brokerId: C.brokerId, userId: C.userId, appId: C.appId, authCode: C.authCode }).catch(() => {}).then(() => t.reqUserLogin({ brokerId: C.brokerId, userId: C.userId, password: C.password }));

let aFront, aSess;
const a = new Trader("./flow-a/", C.td);
a.on("rsp-user-login", (r) => { aFront = r.frontId; aSess = r.sessionId; });
let aSawOrder = 0, aSawTrade = 0;
a.on("rtn-order", (o) => { aSawOrder++; log(`  A sees rtn-order ref=${o.orderRef} front=${o.frontId} sess=${o.sessionId} status=${o.orderStatus}`); });
a.on("rtn-trade", (t) => { aSawTrade++; log(`  A sees rtn-trade ${t.instrumentId} ref=${t.orderRef} front=${t.frontId} sess=${t.sessionId} px=${t.price}`); });
let aReady = false;
a.on("front-connected", async () => { await login(a); log(`A logged in (front=${aFront} sess=${aSess})`); aReady = true; });

const b = new Trader("./flow-b/", C.td);
let bFront, bSess;
b.on("rsp-user-login", (r) => { bFront = r.frontId; bSess = r.sessionId; });
b.on("front-connected", async () => {
  try {
    for (let i = 0; i < 30 && !aReady; i++) await sleep(300);
    await login(b);
    log(`B logged in (front=${bFront} sess=${bSess})`);
    for (let i = 0; i < 30 && !px.au2608; i++) await sleep(300);
    await sleep(1500);
    const buy = Math.round((px.au2608.ask + 0.5) * 50) / 50; // marketable, on 0.02 grid
    log(`B places au2608 BUY 1 @ ${buy} (marketable)`);
    await b.reqOrderInsert({ ...ob, instrumentId: "au2608", direction: "0", combOffsetFlag: "0", limitPrice: buy, volumeTotalOriginal: 1 });
    await sleep(3500);
    const sell = Math.round((px.au2608.bid - 0.5) * 50) / 50;
    log(`B closes au2608 SELL 1 @ ${sell} (close-today)`);
    await b.reqOrderInsert({ ...ob, instrumentId: "au2608", direction: "1", combOffsetFlag: "3", limitPrice: sell, volumeTotalOriginal: 1 });
    await sleep(3500);
    // safety flatten via B
    const pos = await b.reqQryInvestorPosition({ brokerId: C.brokerId, investorId: C.userId });
    for (const p of pos.filter((p) => p.position > 0)) {
      const lng = p.posiDirection === "2";
      await b.reqOrderInsert({ ...ob, instrumentId: p.instrumentId, direction: lng ? "1" : "0", combOffsetFlag: p.todayPosition > 0 ? "3" : "4", limitPrice: lng ? Math.round((px.au2608.bid - 1) * 50) / 50 : Math.round((px.au2608.ask + 1) * 50) / 50, volumeTotalOriginal: p.position });
      await sleep(2500);
    }
    await sleep(1000);
    log(`\nVERDICT: A (front=${aFront} sess=${aSess}) saw ${aSawOrder} order + ${aSawTrade} trade returns from B (front=${bFront} sess=${bSess})`);
    log(aSawOrder > 0 || aSawTrade > 0 ? "=> CROSS-SESSION DELIVERY: YES (a session sees other sessions' orders/trades)" : "=> CROSS-SESSION DELIVERY: NO (only your own session's events)");
  } catch (e) { log("FAILED:", e.message, e.errorId); }
  finally { md.close(); a.close(); b.close(); process.exit(0); }
});

log("connecting...");
setTimeout(() => { try { md.close(); a.close(); b.close(); } catch {} process.exit(1); }, Number(env.CTP_RUN_MS ?? 40000));
