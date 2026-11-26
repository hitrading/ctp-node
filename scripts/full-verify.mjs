// Comprehensive LIVE validation of every native pre-trade risk control, on au2608
// & ag2608 (SHFE metals, night session to 02:30) with real fills. Gate-rejection
// cases use non-marketable prices so that even a (hypothetical) gate miss would
// only rest an order, never fill. Fill-based cases open real 1-lot positions and
// are closed immediately. A finally{} flatten guarantees the account ends flat
// with no working orders, whatever happens.
//
// The native gate is stateless (each rule judges one order against the latest
// quote and static contract metadata), so positions are read back from CTP with
// reqQryInvestorPosition rather than from a binding-side ledger.
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
// Open positions straight from CTP (the account's own facts).
const openPositions = async () =>
  (await td.reqQryInvestorPosition({ brokerId: cfg.brokerId, investorId: cfg.userId })).filter((p) => p.position > 0);
const heldLots = async (id) =>
  (await openPositions()).filter((p) => p.instrumentId === id).reduce((sum, p) => sum + p.position, 0);

function clearAll() {
  td.setRiskLimits({}); // replaces the whole config: every limit disabled
  td.resume();
}

async function flatten(tag) {
  // cancel working orders
  const orders = await td.reqQryOrder({ brokerId: cfg.brokerId, investorId: cfg.userId });
  for (const o of orders.filter((o) => o.orderStatus === "1" || o.orderStatus === "3")) {
    await td.reqOrderAction({ brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: o.instrumentId, exchangeId: o.exchangeId, orderSysId: o.orderSysId, actionFlag: "0" });
    await sleep(700);
  }
  await sleep(800);
  // Close any open position on ANY instrument (not just A/G). Resolve each
  // instrument's tick + a live quote first (the test only pre-subscribes A/G, so a
  // leftover position elsewhere has neither) — then close marketable, with the
  // right offset. An instrument with no live quote (off-session) is skipped with a
  // warning instead of crashing.
  const pos = await openPositions();
  for (const id of [...new Set(pos.map((p) => p.instrumentId))]) {
    if (!inst[id]) {
      const r = (await td.reqQryInstrument({ instrumentId: id }).catch(() => [])).find((x) => x.instrumentId === id);
      if (r) inst[id] = { mult: r.volumeMultiple, tick: r.priceTick };
    }
    if (!px[id]) md.subscribe([id]);
  }
  if (pos.length) await sleep(2500); // let quotes arrive for newly-subscribed instruments
  for (const p of pos) {
    const id = p.instrumentId;
    if (!inst[id] || !px[id] || !(px[id].bid > 0) || !(px[id].ask > 0)) {
      log(`  ** cannot close ${id} (${p.position} lot): no live quote — instrument off-session?`);
      continue;
    }
    const long = p.posiDirection === "2";          // 2 = long, 3 = short
    const dir = long ? "1" : "0";                   // close long -> SELL; close short -> BUY
    const price = long ? mkSell(id) : mkBuy(id);    // marketable, tick-aligned
    // SHFE / INE split close-today ('3') vs close-yesterday ('4'); other exchanges
    // use the generic close ('1'). Handle a mixed today+yesterday position too.
    const split = p.exchangeId === "SHFE" || p.exchangeId === "INE";
    const legs = split
      ? [["3", p.todayPosition], ["4", p.ydPosition]].filter(([, v]) => v > 0)
      : [["1", p.position]];
    for (const [off, vol] of legs) {
      log(`  close ${id} ${long ? "LONG" : "SHORT"} ${vol} @ ${price} offset=${off}`);
      await send(id, dir, off, price, vol, "");
      await sleep(2000);
    }
  }
  await sleep(1500);
  const left = await openPositions();
  log(`  [${tag ?? "flatten"}] ${left.length === 0 ? "account flat" : left.length + " position(s) left: " + left.map((p) => `${p.instrumentId}x${p.position}`).join(", ")}`);
}

td.on("front-connected", async () => {
  try {
    // session() confirms the trading account to the native order gate; a raw
    // reqUserLogin would leave every ordinary order insert refused.
    await td.session({
      brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password,
      appId: cfg.appId, authCode: cfg.authCode,
      sync: { multipliers: [A, G] },
    });
    log("session ok");
    await sleep(1500);
    for (const id of [A, G]) {
      const rows = await td.reqQryInstrument({ instrumentId: id });
      const r = rows.find((x) => x.instrumentId === id);
      inst[id] = { mult: r.volumeMultiple, tick: r.priceTick };
      log(`${id}: mult=${r.volumeMultiple} tick=${r.priceTick} (auto-fed to risk engine)`);
    }
    td.trackMarketData(md); // feed deviation / notional reference
    for (let i = 0; i < 30 && (!px[A] || !px[G]); i++) await sleep(300);
    log(`prices au=${px[A].last} ag=${px[G].last}`);
    await flatten("startup");
    check((await openPositions()).length === 0, "clean start: no open position (reqQryInvestorPosition)");

    // ===== PHASE 1: pre-send gate (non-marketable prices; no fills) =====
    log("== PHASE 1: pre-send gate ==");
    clearAll();
    td.setRiskLimits({ maxOrderVolume: 2 });
    check(/pre-trade risk/i.test(await send(A, "0", "0", restBuy(A), 5, "")), "maxOrderVolume: 5 lots > 2 blocked");

    clearAll();
    td.setRiskLimits({ maxPriceDeviation: 0.02 }); // 2%; ref = last via trackMarketData
    check(/pre-trade risk/i.test(await send(A, "0", "0", rt(px[A].last * 0.85, inst[A].tick), 1, "")), "maxPriceDeviation: -15% vs ref blocked");

    clearAll();
    const auLotNotional = px[A].last * inst[A].mult; // ~949k
    td.setRiskLimits({ maxNotional: auLotNotional / 2 }); // between price*1 and price*mult
    check(/pre-trade risk/i.test(await send(A, "0", "0", restBuy(A), 1, "")), `maxNotional: 1 au lot (~${(auLotNotional / 1e4).toFixed(0)}w) > cap proves multiplier-aware`);

    clearAll();
    td.reduceOnly();
    check(/pre-trade risk/i.test(await send(A, "0", "0", restBuy(A), 1, "")), "kill-switch (reduce-only): open blocked");
    td.resume();

    clearAll();
    td.setRiskLimits({ maxOrdersPerSec: 2, orderBurst: 2 });
    const burst = await Promise.all([1, 2, 3, 4, 5].map(() => send(A, "0", "0", restBuy(A), 1, "")));
    const rateBlocked = burst.filter((r) => /rate limited/i.test(r)).length;
    check(rateBlocked >= 1, `rate limit: ${rateBlocked}/5 throttled (burst 2)`);
    clearAll();
    await flatten("phase1"); // cancel any resting rate-test orders

    // ===== PHASE 2: real open/close cycle, positions read back from CTP =====
    log("== PHASE 2: open/close cycle ==");
    clearAll();
    check(/SENT/.test(await openMkt(A, 1)), "au open 1 submitted (resolves, no hang)");
    await sleep(2500);
    check((await heldLots(A)) >= 1, `au position after the fill: ${await heldLots(A)} lot(s) (reqQryInvestorPosition)`);
    // reduce-only blocks opens but keeps closes available
    td.reduceOnly();
    check(/reduce-only|pre-trade risk/i.test(await openMkt(A, 1)), "reduce-only: further open blocked while holding");
    check(/SENT/.test(await send(A, "1", "3", mkSell(A), 1, "")), "reduce-only: CLOSE passes (closes stay available)");
    td.resume();
    await sleep(2000);
    await flatten();
    await sleep(800);
    check((await heldLots(A)) === 0, `au closed: ${await heldLots(A)} lot(s) left`);

    check(/SENT/.test(await openMkt(G, 1)), "ag open 1 submitted");
    await sleep(2500);
    check((await heldLots(G)) >= 1, `ag position after the fill: ${await heldLots(G)} lot(s) (reqQryInvestorPosition)`);
    await flatten();
    await sleep(800);
    check((await heldLots(G)) === 0, `ag closed: ${await heldLots(G)} lot(s) left`);

    log(`\nRESULT: ${pass} pass, ${fail} fail`);
  } catch (e) {
    fail++;
    log("FAILED:", e.message, "errorId=", e.errorId);
  } finally {
    await flatten("final");
    await sleep(800);
    const posn = await td.reqQryInvestorPosition({ brokerId: cfg.brokerId, investorId: cfg.userId });
    const ordn = await td.reqQryOrder({ brokerId: cfg.brokerId, investorId: cfg.userId });
    log(`FINAL: open positions=${posn.filter((p) => p.position > 0).length}, working orders=${ordn.filter((o) => o.orderStatus === "1" || o.orderStatus === "3").length}`);
    md.close(); td.close();
    process.exit(fail ? 1 : 0);
  }
});

log("connecting...");
setTimeout(() => { try { md.close(); td.close(); } catch {} process.exit(1); }, Number(env.CTP_RUN_MS ?? 180000));
