// Flatten any open position AND verify the automatic risk wiring:
//   - syncMultipliers (contract multiplier from reqQryInstrument)
//   - syncPositions   (open-position cost from reqQryInvestorPosition)
//   - a real close fill updates positionCost via OnRtnTrade (-> ~0)
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
td.on("rtn-order", (o) => log("rtn-order", `${o.orderRef} status=${o.orderStatus} ${o.statusMsg}`));
td.on("rtn-trade", (t) => log("rtn-trade", t.instrumentId, t.price, t.volume));
td.on("front-connected", async () => {
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode }).catch(() => {});
    await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("logged in");
    await sleep(1500);
    log(`syncMultipliers applied ${await td.syncMultipliers([cfg.symbol])}`);
    await sleep(1200);
    const np = await td.syncPositions();
    log(`syncPositions: ${np} open position(s); positionCost=${td.positionCost().toFixed(0)} (auto from CTP)`);
    await sleep(1200);
    const pos = await td.reqQryInvestorPosition({ brokerId: cfg.brokerId, investorId: cfg.userId });
    for (const p of pos) if (p.position > 0) log(`  position ${p.instrumentId} dir=${p.posiDirection} vol=${p.position} openCost=${p.openCost}`);
    if (np > 0) {
      const sellPx = Math.round((last || 3100) - 50);
      log(`flatten: SELL 1 @ ${sellPx} (close-today) ...`);
      td.reqOrderInsert({ brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: cfg.symbol, orderRef: "flat1", direction: "1", combOffsetFlag: "3", combHedgeFlag: "1", orderPriceType: "2", limitPrice: sellPx, volumeTotalOriginal: 1, timeCondition: "3", volumeCondition: "1", contingentCondition: "1", forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0 })
        .then(() => log("close rsp ok")).catch((e) => log("close rsp:", e.message));
      await sleep(3000);
      log(`positionCost after close fill (auto from OnRtnTrade) = ${td.positionCost().toFixed(0)} (expect ~0)`);
    } else {
      log("flat - nothing to close");
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
}, Number(env.CTP_RUN_MS ?? 16000));
