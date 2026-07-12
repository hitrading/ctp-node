// Live validation of the per-instrument position-margin cap, using ag2608 &
// au2608 (real multipliers fetched from CTP). The cap is a pre-send gate, so it
// requires live reference prices and margin schedules. Demonstrates: the same
// cap can block expensive-per-lot gold while admitting cheaper-per-lot silver.
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
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
mkdirSync("flow-td", { recursive: true });
mkdirSync("flow-md", { recursive: true });

const WORKING = new Set(["1", "3"]);
const orderBase = {
  brokerId: cfg.brokerId, investorId: cfg.userId, combHedgeFlag: "1",
  orderPriceType: "2", timeCondition: "3", volumeCondition: "1",
  contingentCondition: "1", forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0,
};
const symbols = ["au2608", "ag2608"];
const seen = new Set();
const md = new MarketData("./flow-md/", cfg.md);
md.on("front-connected", async () => {
  await md.login({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password }).catch(() => {});
  md.subscribe(symbols);
});
md.on("rtn-depth-market-data", (tick) => seen.add(tick.instrumentId));

const td = new Trader("./flow-td/", cfg.td);
td.trackMarketData(md);
td.on("rtn-order", (o) => log("  rtn-order", o.orderRef, "status=" + o.orderStatus, (o.statusMsg || "").trim()));
const cOpen = (id, px, vol) =>
  td.reqOrderInsert({ ...orderBase, instrumentId: id, direction: "0", combOffsetFlag: "0", limitPrice: px, volumeTotalOriginal: vol })
    .then(() => "SENT (passed risk gate)")
    .catch((e) => e.message);

td.on("front-connected", async () => {
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode }).catch(() => {});
    await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("logged in");
    await sleep(1500);

    for (const id of symbols) {
      const rows = await td.reqQryInstrument({ instrumentId: id });
      const row = rows.find((r) => r.instrumentId === id);
      if (row) {
        log(`${id}: multiplier=${row.volumeMultiple} exch=${row.exchangeId} (auto-fed to risk engine)`);
      } else log(`${id}: NOT FOUND (${rows.length} rows)`);
    }
    await td.syncMarginRates(symbols);
    await td.syncOrders();
    log(`positions: ${await td.syncPositions()} (expect 0)`);
    for (let i = 0; i < 30 && symbols.some((id) => !seen.has(id)); i++) await sleep(300);

    td.setInstrumentMarginLimit("au2608", 30000);
    td.setInstrumentMarginLimit("ag2608", 30000);
    log("set per-instrument margin cap: au2608=30k, ag2608=30k (same number)");
    log(`au2608 BUY 1 @560  (expected margin > 30k) -> ${await cOpen("au2608", 560, 1)}`);
    await sleep(800);
    log(`ag2608 BUY 1 @7500 (expected margin < 30k) -> ${await cOpen("ag2608", 7500, 1)}`);
    await sleep(800);
    td.setInstrumentMarginLimit("ag2608", 5000);
    log("tighten ag2608 cap to 5k");
    log(`ag2608 BUY 1 @7500 (expected margin > 5k) -> ${await cOpen("ag2608", 7500, 1)}`);
    await sleep(1200);

    // safety: cancel any working orders the PASS case may have rested
    const orders = await td.reqQryOrder({ brokerId: cfg.brokerId, investorId: cfg.userId });
    const working = orders.filter((o) => WORKING.has(o.orderStatus));
    log(`working orders left: ${working.length}`);
    for (const o of working) {
      log(`  cancel ${o.instrumentId} ref=${o.orderRef}`);
      await td.reqOrderAction({ brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: o.instrumentId, exchangeId: o.exchangeId, orderSysId: o.orderSysId, actionFlag: "0" });
      await sleep(900);
    }
  } catch (e) {
    log("FAILED:", e.message, "errorId=", e.errorId);
  }
});

log("connecting...");
setTimeout(() => {
  md.close();
  td.close();
  process.exit(0);
}, Number(env.CTP_RUN_MS ?? 45000));
