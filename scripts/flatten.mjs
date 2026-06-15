// Definitive live open->close cycle. Verifies:
//   - syncMultipliers reliably applies the multiplier (cold-start retry)
//   - reqOrderInsert RESOLVES on submit (no hang on a filled order)
//   - positionCost from pure onTrade == price*vol*multiplier (31800, not 3180)
//   - positionCost returns to 0 after the close; account ends flat
// Listens for every order event so any rejection is visible.
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

const order = (extra) => ({
  brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: cfg.symbol,
  combHedgeFlag: "1", orderPriceType: "2", volumeTotalOriginal: 1,
  timeCondition: "3", volumeCondition: "1", contingentCondition: "1",
  forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0, ...extra,
});

const td = new Trader("./flow-td/", cfg.td);
td.on("rtn-order", (o) => log("  rtn-order", o.orderRef, "status=" + o.orderStatus, o.statusMsg));
td.on("rtn-trade", (t) => log("  rtn-trade", t.instrumentId, "px=" + t.price, "vol=" + t.volume, "offset=" + t.offsetFlag));
td.on("rsp-order-insert", (d, o) => log("  RSP-ORDER-INSERT err=", JSON.stringify(o.rspInfo)));
td.on("err-rtn-order-insert", (d, o) => log("  ERR-RTN-ORDER-INSERT err=", JSON.stringify(o.rspInfo)));
td.on("front-connected", async () => {
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode }).catch(() => {});
    await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("logged in");
    await sleep(1500);
    log(`syncMultipliers applied: ${await td.syncMultipliers([cfg.symbol])} contract(s)`);
    log(`syncPositions: ${await td.syncPositions()} open position(s); positionCost=${td.positionCost().toFixed(0)} (expect 0)`);

    await sleep(1200);
    const buyPx = Math.round((last || 3200) + 80);
    log(`OPEN BUY 1 @ ${buyPx} (marketable, orderRef auto-assigned) ...`);
    await td.reqOrderInsert(order({ direction: "0", combOffsetFlag: "0", limitPrice: buyPx }));
    log("  -> open await RESOLVED (submitted, no hang)");
    await sleep(2500);
    log(`positionCost after open (pure onTrade) = ${td.positionCost().toFixed(0)} (expect 31800 = 3180*1*10)`);

    await sleep(1000);
    const sellPx = Math.round((last || 3100) - 80);
    log(`CLOSE SELL 1 @ ${sellPx} (close-today, orderRef auto-assigned) ...`);
    await td.reqOrderInsert(order({ direction: "1", combOffsetFlag: "3", limitPrice: sellPx }));
    log("  -> close await RESOLVED");
    await sleep(2500);
    log(`positionCost after close (onTrade release) = ${td.positionCost().toFixed(0)} (expect 0)`);

    await sleep(1200);
    log(`syncPositions confirm: ${await td.syncPositions()} open position(s) -> account flat`);
  } catch (e) {
    log("FAILED:", e.message, "errorId=", e.errorId);
  }
});

log("connecting...");
setTimeout(() => {
  md.close();
  td.close();
  process.exit(0);
}, Number(env.CTP_RUN_MS ?? 26000));
