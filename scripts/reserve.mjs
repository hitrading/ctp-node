// Live validation of in-flight reservation (burst protection), works with the
// market closed: an order the front accepts (rc=0) reserves immediately; the
// exchange rejection arrives a few ms later. So a 2nd open fired right after the
// 1st (still in-flight) must be blocked by the reservation, and a 3rd open after
// the rejection (reservation released) must pass again.
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
const open = () =>
  td.reqOrderInsert({ ...orderBase, instrumentId: "au2608", direction: "0", combOffsetFlag: "0", limitPrice: 560, volumeTotalOriginal: 1 })
    .then(() => "SENT (front accepted -> reserved)")
    .catch((e) => e.message);

td.on("front-connected", async () => {
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode }).catch(() => {});
    await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("logged in");
    await sleep(1500);
    const rows = await td.reqQryInstrument({ instrumentId: "au2608" });
    const row = rows.find((r) => r.instrumentId === "au2608");
    if (row) log(`au2608 mult=${row.volumeMultiple} (auto-fed to risk engine from this query)`);
    await sleep(1100);
    log(`positions: ${await td.syncPositions()} (expect 0)`);

    td.setMaxPosition("au2608", { long: 1 }); // at most 1 long lot
    log("setMaxPosition(au2608, { long: 1 })");

    log(`open #1                       -> ${await open()}`);
    log(`open #2 (while #1 in-flight)  -> ${await open()}   <-- expect BLOCKED by reservation`);
    await sleep(2500); // let #1's exchange rejection arrive -> reservation released
    log(`open #3 (after #1 released)   -> ${await open()}   <-- expect SENT again`);
    await sleep(2500);

    const orders = await td.reqQryOrder({ brokerId: cfg.brokerId, investorId: cfg.userId });
    const working = orders.filter((o) => WORKING.has(o.orderStatus));
    log(`working orders left: ${working.length}`);
    for (const o of working) {
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
