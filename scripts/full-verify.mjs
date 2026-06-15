// Comprehensive LIVE validation of every pre-trade risk control, on au2608 &
// ag2608 (SHFE metals, night session to 02:30) with real fills. Gate-rejection
// cases use non-marketable prices so that even a (hypothetical) gate miss would
// only rest an order, never fill. Fill-based cases open real 1-lot positions and
// are closed immediately. A finally{} flatten guarantees the account ends flat
// with no working orders, whatever happens.
import { MarketData, Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";
import process from "node:process";

const env = process.env;
const cfg = {
  brokerId: env.CTP_BROKER ?? "9999",
  userId: env.CTP_USER ?? "",
  password: env.CTP_PASS ?? "",
  appId: env.CTP_APPID ?? "simnow_client_test",
  authCode: env.CTP_AUTHCODE ?? "0000000000000000",
  md: env.CTP_MD_FRONT,
  td: env.CTP_TD_FRONT,
};
const A = "au2608", G = "ag2608";
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
mkdirSync("flow-md", { recursive: true });
mkdirSync("flow-td", { recursive: true });

let pass = 0, fail = 0;
const check = (cond, msg) => (cond ? (pass++, log("  PASS:", msg)) : (fail++, log("  ** FAIL:", msg)));

const px = {};
const md = new MarketData("./flow-md/", cfg.md);
md.on("front-connected", async () => {
  await md.login({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password }).catch(() => {});
  md.subscribe([A, G]);
});
md.on("rtn-depth-market-data", (t) => (px[t.instrumentId] = { bid: t.bidPrice1, ask: t.askPrice1, last: t.lastPrice }));

const inst = {}; // id -> { mult, tick }
const td = new Trader("./flow-td/", cfg.td);
td.on("rtn-trade", (t) => log("    fill", t.instrumentId, "px=" + t.price, "vol=" + t.volume, "off=" + t.offsetFlag));

const orderBase = {
  brokerId: cfg.brokerId, investorId: cfg.userId, combHedgeFlag: "1",
  orderPriceType: "2", timeCondition: "3", volumeCondition: "1",
  contingentCondition: "1", forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0,
};
const rt = (p, tick) => Number((Math.round(p / tick) * tick).toFixed(6));
const mkBuy = (id) => rt(px[id].ask + 3 * inst[id].tick, inst[id].tick);   // marketable
const mkSell = (id) => rt(px[id].bid - 3 * inst[id].tick, inst[id].tick);  // marketable
const restBuy = (id) => rt(px[id].last * 0.96, inst[id].tick);             // rests (below mkt, above limit)
const send = (id, dir, off, price, vol, ref) =>
  td.reqOrderInsert({ ...orderBase, instrumentId: id, direction: dir, combOffsetFlag: off, limitPrice: price, volumeTotalOriginal: vol, ...(ref ? { orderRef: ref } : {}) })
    .then(() => "SENT").catch((e) => e.message);
const openMkt = (id, vol) => send(id, "0", "0", mkBuy(id), vol);
const cost = () => td.positionCost();

function clearAll() {
  td.riskSet({}); // clears scalar caps incl. global maxPositionCost
  td.resume();
  for (const id of [A, G]) { td.setMaxPosition(id, 0); td.setMaxPositionCost(id, 0); }
}

async function flatten(tag) {
  // cancel working orders
  const orders = await td.reqQryOrder({ brokerId: cfg.brokerId, investorId: cfg.userId });
  for (const o of orders.filter((o) => o.orderStatus === "1" || o.orderStatus === "3")) {
    await td.reqOrderAction({ brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: o.instrumentId, exchangeId: o.exchangeId, orderSysId: o.orderSysId, actionFlag: "0" });
    await sleep(700);
  }
  await sleep(800);
  // close any open positions, marketable, correct close-today/yesterday offset
  const pos = await td.reqQryInvestorPosition({ brokerId: cfg.brokerId, investorId: cfg.userId });
  for (const p of pos.filter((p) => p.position > 0)) {
    const long = p.posiDirection === "2";
    const off = p.todayPosition > 0 ? "3" : "4";
    const price = long ? mkSell(p.instrumentId) : mkBuy(p.instrumentId);
    await send(p.instrumentId, long ? "1" : "0", off, price, p.position, "");
    await sleep(2200);
  }
  if (tag) log(`  [${tag}] flattened`);
}

td.on("front-connected", async () => {
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode }).catch(() => {});
    await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("logged in");
    await sleep(1500);
    for (const id of [A, G]) {
      const rows = await td.reqQryInstrument({ instrumentId: id });
      const r = rows.find((x) => x.instrumentId === id);
      inst[id] = { mult: r.volumeMultiple, tick: r.priceTick };
      td.setMultiplier(id, r.volumeMultiple);
      log(`${id}: mult=${r.volumeMultiple} tick=${r.priceTick}`);
      await sleep(1100);
    }
    td.trackMarketData(md); // feed deviation reference
    for (let i = 0; i < 30 && (!px[A] || !px[G]); i++) await sleep(300);
    log(`prices au=${px[A].last} ag=${px[G].last}`);
    await flatten("startup");
    await td.syncPositions();
    check(Math.abs(cost()) < 1, `clean start: positionCost=${cost().toFixed(0)}`);

    // ===== PHASE 1: pre-send gate (non-marketable prices; no fills) =====
    log("== PHASE 1: pre-send gate ==");
    clearAll();
    td.riskSet({ maxOrderVolume: 2 });
    check(/pre-trade risk/i.test(await send(A, "0", "0", restBuy(A), 5, "")), "maxOrderVolume: 5 lots > 2 blocked");

    clearAll();
    td.riskSet({ maxPriceDeviation: 0.02 }); // 2%; ref = last via trackMarketData
    check(/pre-trade risk/i.test(await send(A, "0", "0", rt(px[A].last * 0.85, inst[A].tick), 1, "")), "maxPriceDeviation: -15% vs ref blocked");

    clearAll();
    const auLotNotional = px[A].last * inst[A].mult; // ~949k
    td.riskSet({ maxNotional: auLotNotional / 2 }); // between price*1 and price*mult
    check(/pre-trade risk/i.test(await send(A, "0", "0", restBuy(A), 1, "")), `maxNotional: 1 au lot (~${(auLotNotional / 1e4).toFixed(0)}w) > cap proves multiplier-aware`);

    clearAll();
    td.halt();
    check(/pre-trade risk/i.test(await send(A, "0", "0", restBuy(A), 1, "")), "kill-switch (halt): blocked");
    td.resume();

    clearAll();
    td.riskSet({ maxOrdersPerSec: 2, orderBurst: 2 });
    const burst = await Promise.all([1, 2, 3, 4, 5].map(() => send(A, "0", "0", restBuy(A), 1, "")));
    const rateBlocked = burst.filter((r) => /rate limited/i.test(r)).length;
    check(rateBlocked >= 1, `rate limit: ${rateBlocked}/5 throttled (burst 2)`);
    clearAll();
    await flatten("phase1"); // cancel any resting rate-test orders

    // ===== PHASE 2: real open/close cycle, position-cost tracking =====
    log("== PHASE 2: open/close cycle ==");
    clearAll();
    check(/SENT/.test(await openMkt(A, 1)), "au open 1 submitted (resolves, no hang)");
    await sleep(2500);
    const auCost = cost();
    check(auCost > 900000 && auCost < 1000000, `au positionCost=${auCost.toFixed(0)} ~= price*1*1000`);
    await flatten();
    await sleep(800);
    check(Math.abs(cost()) < 1, `au closed: positionCost=${cost().toFixed(0)}`);

    check(/SENT/.test(await openMkt(G, 1)), "ag open 1 submitted");
    await sleep(2500);
    const agCost = cost();
    check(agCost > 230000 && agCost < 280000, `ag positionCost=${agCost.toFixed(0)} ~= price*1*15`);
    await flatten();
    await sleep(800);
    check(Math.abs(cost()) < 1, `ag closed: positionCost=${cost().toFixed(0)}`);

    // ===== PHASE 3: per-side lot cap with real fills =====
    log("== PHASE 3: lot cap ==");
    clearAll();
    td.setMaxPosition(A, { long: 2 });
    check(/SENT/.test(await openMkt(A, 1)), "lot cap: open 1/2 ok");
    await sleep(2200);
    check(/SENT/.test(await openMkt(A, 1)), "lot cap: open 2/2 ok");
    await sleep(2200);
    check(/position volume/i.test(await openMkt(A, 1)), "lot cap: open 3rd (held 2, cap 2) blocked");
    // close is exempt from the cap even though it's full:
    check(/SENT/.test(await send(A, "1", "3", mkSell(A), 1, "")), "lot cap: CLOSE passes despite full cap (close exempt)");
    await flatten();
    await sleep(800);
    check(Math.abs(cost()) < 1, `lot cap: flat after, positionCost=${cost().toFixed(0)}`);

    // ===== PHASE 4: per-instrument cost cap with real position =====
    log("== PHASE 4: per-instrument cost cap ==");
    clearAll();
    td.setMaxPositionCost(A, auLotNotional * 1.5); // ~1.5 lots
    check(/SENT/.test(await openMkt(A, 1)), "instr cost cap: open 1 (~1 lot) ok");
    await sleep(2500);
    check(/position cost/i.test(await openMkt(A, 1)), "instr cost cap: open 2 (~2 lots > 1.5) blocked");
    await flatten();
    await sleep(800);

    // ===== PHASE 5: global cost cap across au + ag =====
    log("== PHASE 5: global cost cap ==");
    clearAll();
    td.riskSet({ maxPositionCost: px[A].last * inst[A].mult + px[G].last * inst[G].mult * 1.5 }); // ~ 1 au + 1.5 ag
    check(/SENT/.test(await openMkt(A, 1)), "global cap: au ok");
    await sleep(2500);
    check(/SENT/.test(await openMkt(G, 1)), "global cap: + ag ok (still under)");
    await sleep(2500);
    check(/position cost/i.test(await openMkt(G, 1)), "global cap: + 2nd ag blocked (au+2ag over cap)");
    await flatten();
    await sleep(800);
    check(Math.abs(cost()) < 1, `global cap: flat after, positionCost=${cost().toFixed(0)}`);

    // ===== PHASE 6: in-flight reservation (resting order) =====
    log("== PHASE 6: in-flight reservation ==");
    clearAll();
    td.setMaxPosition(A, { long: 1 });
    check(/SENT/.test(await send(A, "0", "0", restBuy(A), 1, "")), "reservation: resting open 1 sent (reserves)");
    await sleep(800);
    check(/position volume/i.test(await openMkt(A, 1)), "reservation: 2nd open blocked while 1st in-flight (not yet filled)");
    // cancel the resting order -> reservation released
    const ords = await td.reqQryOrder({ brokerId: cfg.brokerId, investorId: cfg.userId });
    for (const o of ords.filter((o) => o.orderStatus === "1" || o.orderStatus === "3")) {
      await td.reqOrderAction({ brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: o.instrumentId, exchangeId: o.exchangeId, orderSysId: o.orderSysId, actionFlag: "0" });
      await sleep(900);
    }
    await sleep(800);
    check(/SENT/.test(await openMkt(A, 1)), "reservation: open passes after cancel (reservation released)");
    await sleep(2500); // that one fills -> held 1
    check(/position volume/i.test(await openMkt(A, 1)), "reservation: held 1 (=cap) -> next open blocked (reserve->held, no gap/double-count)");
    await flatten();
    await sleep(800);
    check(Math.abs(cost()) < 1, `reservation: flat after, positionCost=${cost().toFixed(0)}`);

    log(`\nRESULT: ${pass} pass, ${fail} fail`);
  } catch (e) {
    log("FAILED:", e.message, "errorId=", e.errorId);
  } finally {
    await flatten("final");
    await sleep(800);
    const posn = await td.reqQryInvestorPosition({ brokerId: cfg.brokerId, investorId: cfg.userId });
    const ordn = await td.reqQryOrder({ brokerId: cfg.brokerId, investorId: cfg.userId });
    log(`FINAL: open positions=${posn.filter((p) => p.position > 0).length}, working orders=${ordn.filter((o) => o.orderStatus === "1" || o.orderStatus === "3").length}`);
    md.close(); td.close();
    process.exit(0);
  }
});

log("connecting...");
setTimeout(() => { try { md.close(); td.close(); } catch {} process.exit(1); }, Number(env.CTP_RUN_MS ?? 180000));
