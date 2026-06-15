// Safety: query today's orders, show status, and cancel any still working
// (queueing) so nothing rests into the next session. Then confirm none remain.
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

const WORKING = new Set(["1", "3"]); // PartTradedQueueing, NoTradeQueueing
const td = new Trader("./flow-td/", cfg.td);
td.on("rtn-order", (o) => log("  rtn-order", o.orderRef, "status=" + o.orderStatus, o.statusMsg));
td.on("front-connected", async () => {
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode }).catch(() => {});
    await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("logged in");
    await sleep(1500);

    const orders = await td.reqQryOrder({ brokerId: cfg.brokerId, investorId: cfg.userId });
    log(`reqQryOrder rows=${orders.length}`);
    for (const o of orders) {
      log("  order", o.orderRef, "sysId=" + (o.orderSysId || "-").trim(), "status=" + o.orderStatus, (o.statusMsg || "").trim(), `traded=${o.volumeTraded}/${o.volumeTotalOriginal}`);
    }
    const working = orders.filter((o) => WORKING.has(o.orderStatus));
    log(`working (queueing) orders: ${working.length}`);
    for (const o of working) {
      log(`CANCEL ${o.instrumentId} ref=${o.orderRef} sysId=${(o.orderSysId || "").trim()}`);
      await td.reqOrderAction({
        brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: o.instrumentId,
        exchangeId: o.exchangeId, orderSysId: o.orderSysId, actionFlag: "0", // delete
      });
      await sleep(1000);
    }

    if (working.length) {
      await sleep(1500);
      const after = await td.reqQryOrder({ brokerId: cfg.brokerId, investorId: cfg.userId });
      const stillWorking = after.filter((o) => WORKING.has(o.orderStatus));
      log(`AFTER CANCEL: working orders = ${stillWorking.length}`);
    }
  } catch (e) {
    log("FAILED:", e.message, "errorId=", e.errorId);
  }
});

log("connecting...");
setTimeout(() => {
  td.close();
  process.exit(0);
}, Number(env.CTP_RUN_MS ?? 16000));
