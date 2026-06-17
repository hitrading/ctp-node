// flatten-all.mjs - close EVERY open position on the account (any instrument),
// marketable, with the correct per-exchange close offset; cancel working orders
// first. Best-effort: skips (with a warning) anything with no live quote
// (off-session / illiquid). Credentials come ONLY from the environment.
import { MarketData, Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";
const env = process.env;
const cfg = {
  brokerId: env.CTP_BROKER ?? "9999", userId: env.CTP_USER, password: env.CTP_PASS,
  appId: env.CTP_APPID ?? "simnow_client_test", authCode: env.CTP_AUTHCODE ?? "0000000000000000",
  md: env.CTP_MD_FRONT ?? "tcp://182.254.243.31:30012", td: env.CTP_TD_FRONT ?? "tcp://182.254.243.31:30002",
};
if (!cfg.userId || !cfg.password) { console.error("Set CTP_USER and CTP_PASS."); process.exit(2); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
mkdirSync("flow-md", { recursive: true }); mkdirSync("flow-td", { recursive: true });

const px = {};
const md = new MarketData("./flow-md/", cfg.md);
md.on("front-connected", async () => { await md.login({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password }).catch(() => {}); });
md.on("rtn-depth-market-data", (t) => { px[t.instrumentId] = { bid: t.bidPrice1, ask: t.askPrice1 }; });

const td = new Trader("./flow-td/", cfg.td);
td.on("rtn-trade", (tr) => log(`  FILL close ${tr.instrumentId} ${tr.volume}@${tr.price}`));
const base = { brokerId: cfg.brokerId, investorId: cfg.userId, combHedgeFlag: "1", orderPriceType: "2", timeCondition: "3", volumeCondition: "1", contingentCondition: "1", forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0 };

td.on("front-connected", async () => {
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode }).catch(() => {});
    await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    await td.reqSettlementInfoConfirm({ brokerId: cfg.brokerId, investorId: cfg.userId }).catch(() => {});
    await sleep(1200);
    const orders = await td.reqQryOrder({ brokerId: cfg.brokerId, investorId: cfg.userId });
    for (const o of orders.filter((o) => o.orderStatus === "1" || o.orderStatus === "3")) {
      await td.reqOrderAction({ brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: o.instrumentId, exchangeId: o.exchangeId, orderSysId: o.orderSysId, actionFlag: "0" }).catch(() => {});
      await sleep(500);
    }
    const pos = (await td.reqQryInvestorPosition({ brokerId: cfg.brokerId, investorId: cfg.userId })).filter((p) => p.position > 0);
    log(`open positions to flatten: ${pos.length}`);
    if (pos.length) {
      md.subscribe([...new Set(pos.map((p) => p.instrumentId))]);
      await sleep(3500); // let quotes arrive
      for (const p of pos) {
        const id = p.instrumentId, q = px[id];
        if (!q || !(q.bid > 0) || !(q.ask > 0) || q.bid >= 1e300) { log(`  ** skip ${id} x${p.position}: no live quote (off-session/illiquid)`); continue; }
        const long = p.posiDirection === "2";
        const dir = long ? "1" : "0";                 // close long -> SELL; close short -> BUY
        const price = long ? q.bid : q.ask;           // marketable, tick-aligned
        const split = p.exchangeId === "SHFE" || p.exchangeId === "INE";
        const legs = split ? [["3", p.todayPosition], ["4", p.ydPosition]].filter(([, v]) => v > 0) : [["1", p.position]];
        for (const [off, vol] of legs) {
          log(`  close ${id} ${long ? "LONG" : "SHORT"} ${vol} @ ${price} offset=${off}`);
          await td.reqOrderInsert({ ...base, instrumentId: id, direction: dir, combOffsetFlag: off, limitPrice: price, volumeTotalOriginal: vol }).catch((e) => log(`    refused: ${e.message}`));
          await sleep(1500);
        }
      }
    }
    await sleep(2500);
    const left = (await td.reqQryInvestorPosition({ brokerId: cfg.brokerId, investorId: cfg.userId })).filter((p) => p.position > 0);
    log(`RESULT: ${left.length === 0 ? "account FLAT" : left.length + " left: " + left.map((p) => `${p.instrumentId}x${p.position}`).join(", ")}`);
  } catch (e) { log("FAILED:", e.message, "errorId", e.errorId); }
  setTimeout(() => { md.close(); td.close(); process.exit(0); }, 800);
});
setTimeout(() => process.exit(0), 120000);
