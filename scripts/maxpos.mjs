// Live verification of the per-instrument max-position (lots) cap.
// Caps rb2610 at 2 lots, then: open 1 (ok) -> open 2 (BLOCKED, 1+2>2) ->
// open 1 (ok, now 2) -> open 1 (BLOCKED, 2+1>2) -> flatten. Ends flat.
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
const openN = (n) =>
  td.reqOrderInsert(order({ direction: "0", combOffsetFlag: "0", volumeTotalOriginal: n, limitPrice: Math.round((last || 3200) + 80) }))
    .then(() => "SENT")
    .catch((e) => e.message);

const td = new Trader("./flow-td/", cfg.td);
td.on("rtn-trade", (t) => log("  rtn-trade", t.instrumentId, "px=" + t.price, "vol=" + t.volume, "offset=" + t.offsetFlag));
td.on("front-connected", async () => {
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode }).catch(() => {});
    await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("logged in");
    await sleep(1500);
    await td.syncMultipliers([cfg.symbol]);
    log(`start positions: ${await td.syncPositions()} (positionCost=${td.positionCost().toFixed(0)})`);

    td.setMaxPosition(cfg.symbol, 2); // cap: 2 lots per side
    log("setMaxPosition(rb2610, 2)");

    await sleep(1000);
    log(`open 1 -> ${await openN(1)} (expect SENT)`);
    await sleep(2500); // let the fill land so the cap sees held=1
    log(`  held long now ~1, positionCost=${td.positionCost().toFixed(0)}`);
    log(`open 2 -> ${await openN(2)} (expect BLOCKED: 1+2>2)`);
    await sleep(500);
    log(`open 1 -> ${await openN(1)} (expect SENT)`);
    await sleep(2500); // fill -> held=2
    log(`  held long now ~2, positionCost=${td.positionCost().toFixed(0)}`);
    log(`open 1 -> ${await openN(1)} (expect BLOCKED: 2+1>2)`);

    // flatten whatever we hold
    await sleep(1200);
    const n = await td.syncPositions();
    if (n > 0) {
      const sellPx = Math.round((last || 3100) - 80);
      log(`CLOSE long x2 @ ${sellPx} (close-today) ...`);
      await td.reqOrderInsert(order({ direction: "1", combOffsetFlag: "3", volumeTotalOriginal: 2, limitPrice: sellPx }));
      await sleep(2500);
    }
    await sleep(1200);
    log(`FINAL positions: ${await td.syncPositions()} (positionCost=${td.positionCost().toFixed(0)}) -> account flat`);
  } catch (e) {
    log("FAILED:", e.message, "errorId=", e.errorId);
  }
});

log("connecting...");
setTimeout(() => {
  md.close();
  td.close();
  process.exit(0);
}, Number(env.CTP_RUN_MS ?? 30000));
