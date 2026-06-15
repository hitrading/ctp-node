// Live validation of the per-instrument max-position-COST cap, using ag2608 &
// au2608 (real multipliers fetched from CTP). The cap is a pre-send gate, so it
// validates with the market closed: the BLOCK cases send no order at all; the
// PASS case reaches the API (then the exchange rejects it out of session).
// Demonstrates: same numeric cap, au (expensive/lot) blocked vs ag (cheap)
// allowed -> the cap is cost-aware (price*multiplier) and per-instrument.
import { Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";
import process from "node:process";

const env = process.env;
const cfg = {
  brokerId: env.CTP_BROKER ?? "9999",
  userId: env.CTP_USER ?? "",
  password: env.CTP_PASS ?? "",
  appId: env.CTP_APPID ?? "simnow_client_test",
  authCode: env.CTP_AUTHCODE ?? "0000000000000000",
  td: env.CTP_TD_FRONT,
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
mkdirSync("flow-td", { recursive: true });

const WORKING = new Set(["1", "3"]);
const orderBase = {
  brokerId: cfg.brokerId, investorId: cfg.userId, combHedgeFlag: "1",
  orderPriceType: "2", timeCondition: "3", volumeCondition: "1",
  contingentCondition: "1", forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0,
};
const td = new Trader("./flow-td/", cfg.td);
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

    for (const id of ["au2608", "ag2608"]) {
      const rows = await td.reqQryInstrument({ instrumentId: id });
      const row = rows.find((r) => r.instrumentId === id);
      if (row) {
        td.setMultiplier(id, row.volumeMultiple);
        log(`${id}: multiplier=${row.volumeMultiple} exch=${row.exchangeId}`);
      } else log(`${id}: NOT FOUND (${rows.length} rows)`);
      await sleep(1100);
    }
    log(`positions: ${await td.syncPositions()} (expect 0)`);

    td.setMaxPositionCost("au2608", 300000);
    td.setMaxPositionCost("ag2608", 300000);
    log("set per-instrument cost cap: au2608=300k, ag2608=300k (same number)");
    log(`au2608 BUY 1 @560  (cost ~560k > 300k)  -> ${await cOpen("au2608", 560, 1)}`);
    await sleep(800);
    log(`ag2608 BUY 1 @7500 (cost ~112.5k < 300k) -> ${await cOpen("ag2608", 7500, 1)}`);
    await sleep(800);
    td.setMaxPositionCost("ag2608", 50000);
    log("tighten ag2608 cap to 50k");
    log(`ag2608 BUY 1 @7500 (cost ~112.5k > 50k)  -> ${await cOpen("ag2608", 7500, 1)}`);
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
  td.close();
  process.exit(0);
}, Number(env.CTP_RUN_MS ?? 22000));
