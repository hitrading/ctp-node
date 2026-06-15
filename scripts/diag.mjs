// Verbose live diagnostic + safe flatten. Logs every order-related event so
// rejections (rsp-order-insert / err-rtn-order-insert) are visible, prints the
// instrument multiplier and raw position rows, then closes any open position
// using the correct close-today/close-yesterday offset. Leaves the account flat.
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
  symbol: env.CTP_SYMBOL ?? "rb2610",
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
mkdirSync("flow-md", { recursive: true });
mkdirSync("flow-td", { recursive: true });

const md = new MarketData("./flow-md/", cfg.md);
let last = 0;
md.on("front-connected", async () => {
  await md.login({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password }).catch(() => {});
  md.subscribe([cfg.symbol]);
});
md.on("rtn-depth-market-data", (t) => (last = t.lastPrice));

const td = new Trader("./flow-td/", cfg.td);
td.on("rtn-order", (o) => log("  rtn-order", o.orderRef, "status=" + o.orderStatus, o.statusMsg));
td.on("rtn-trade", (t) => log("  rtn-trade", t.instrumentId, "px=" + t.price, "vol=" + t.volume, "offset=" + t.offsetFlag, "dir=" + t.direction));
td.on("rsp-order-insert", (d, o) => log("  RSP-ORDER-INSERT err=", JSON.stringify(o.rspInfo), "ref=", d && d.orderRef));
td.on("err-rtn-order-insert", (d, o) => log("  ERR-RTN-ORDER-INSERT err=", JSON.stringify(o.rspInfo), "ref=", d && d.orderRef));

const orderBase = {
  brokerId: cfg.brokerId, investorId: cfg.userId,
  combHedgeFlag: "1", orderPriceType: "2",
  timeCondition: "3", volumeCondition: "1", contingentCondition: "1",
  forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0,
};

const queryPositions = () =>
  td.reqQryInvestorPosition({ brokerId: cfg.brokerId, investorId: cfg.userId });

td.on("front-connected", async () => {
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode }).catch(() => {});
    await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("logged in; investor=", cfg.userId);
    await sleep(1500);

    // (1) diagnose the multiplier
    const inst = await td.reqQryInstrument({ instrumentId: cfg.symbol });
    log("reqQryInstrument rows=", inst.length);
    for (const r of inst) log("  inst", r.instrumentId, "exch=" + r.exchangeId, "volumeMultiple=" + r.volumeMultiple, "priceTick=" + r.priceTick);
    await sleep(1100);

    // (2) raw positions
    const pos = await queryPositions();
    log("reqQryInvestorPosition rows=", pos.length);
    for (const p of pos) log("  pos", p.instrumentId, "dir=" + p.posiDirection, "pos=" + p.position, "today=" + p.todayPosition, "yd=" + p.ydPosition, "openCost=" + p.openCost);

    // (3) flatten any open position with the correct offset
    const ref = last || 3180;
    for (const p of pos) {
      if (!(p.position > 0)) continue;
      const long = p.posiDirection === "2";
      const dir = long ? "1" : "0";                 // close long -> sell; close short -> buy
      const offset = p.todayPosition > 0 ? "3" : "4"; // SHFE: close-today vs close-yesterday
      const px = long ? Math.round(ref - 80) : Math.round(ref + 80);
      log(`CLOSE ${long ? "long" : "short"} ${p.instrumentId} x${p.position} dir=${dir} offset=${offset} @ ${px}`);
      await td.reqOrderInsert({
        ...orderBase, instrumentId: p.instrumentId, direction: dir,
        combOffsetFlag: offset, limitPrice: px, volumeTotalOriginal: p.position,
        orderRef: "flat-" + p.instrumentId.slice(-4),
      });
      log("  -> close submit RESOLVED");
      await sleep(3500);
    }

    // (4) confirm flat
    await sleep(1500);
    const pos2 = await queryPositions();
    const open = pos2.filter((p) => p.position > 0);
    log("AFTER FLATTEN: open positions =", open.length);
    for (const p of open) log("  STILL OPEN", p.instrumentId, "pos=" + p.position, "today=" + p.todayPosition, "yd=" + p.ydPosition);
  } catch (e) {
    log("FAILED:", e.message, "errorId=", e.errorId);
  }
});

log("connecting...");
setTimeout(() => {
  md.close();
  td.close();
  process.exit(0);
}, Number(env.CTP_RUN_MS ?? 32000));
