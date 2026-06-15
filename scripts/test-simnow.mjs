// Live SimNow integration smoke. Reads credentials from env so nothing secret
// is committed. Run, e.g.:
//   CTP_USER=xxxx CTP_PASS=yyyy node scripts/test-simnow.mjs
//
// Defaults target SimNow (brokerId 9999, the standard test AppID/AuthCode).
// Override any of: CTP_BROKER CTP_USER CTP_PASS CTP_APPID CTP_AUTHCODE
//                  CTP_MD_FRONT CTP_TD_FRONT CTP_SYMBOL CTP_PLACE_ORDER
import { MarketData, Trader } from "../dist/index.js";
import { mkdirSync, appendFileSync, writeFileSync } from "node:fs";
import process from "node:process";

const LOGF = "simnow.out.log";
try { writeFileSync(LOGF, `=== run ${new Date().toISOString()} ===\n`); } catch {}
const flog = (line) => { try { appendFileSync(LOGF, line + "\n"); } catch {} };
process.on("uncaughtException", (e) => flog("UNCAUGHT: " + (e?.stack || e)));
process.on("unhandledRejection", (e) => flog("UNHANDLED: " + (e?.stack || e?.message || e)));

const env = process.env;
const cfg = {
  brokerId: env.CTP_BROKER ?? "9999",
  userId: env.CTP_USER ?? "",
  password: env.CTP_PASS ?? "",
  appId: env.CTP_APPID ?? "simnow_client_test",
  authCode: env.CTP_AUTHCODE ?? "0000000000000000",
  mdFront: env.CTP_MD_FRONT ?? "tcp://180.168.146.187:10211",
  tdFront: env.CTP_TD_FRONT ?? "tcp://180.168.146.187:10201",
  symbol: env.CTP_SYMBOL ?? "rb2610",
  placeOrder: env.CTP_PLACE_ORDER === "1",
  arm: env.CTP_ARM === "1",
  devDemo: env.CTP_DEV_DEMO === "1",
  riskLive: env.CTP_RISK_LIVE === "1",
  skipAuth: env.CTP_SKIP_AUTH === "1",
};

if (!cfg.userId || !cfg.password) {
  console.error("Set CTP_USER and CTP_PASS (SimNow account). See header for all env vars.");
  process.exit(2);
}

mkdirSync("flow-md", { recursive: true });
mkdirSync("flow-td", { recursive: true });
const log = (...a) => {
  const line = new Date().toISOString().slice(11, 23) + " " + a.map((x) => (typeof x === "object" ? JSON.stringify(x) : String(x))).join(" ");
  console.log(line);
  flog(line);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------------- Market data ----------------
const md = new MarketData("./flow-md/", cfg.mdFront);
md.on("rsp-user-login", (d, o) => log(`MD [event] rsp-user-login: reqId=${o.requestId} isLast=${o.isLast}`, o.rspInfo ? o.rspInfo.errorMsg : `ok tradingDay=${d?.tradingDay}`));
md.on("rsp-error", (d, o) => log("MD [event] rsp-error:", JSON.stringify(o.rspInfo)));
md.on("front-connected", async () => {
  log("MD front-connected; logging in...");
  try {
    const r = await md.login({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("MD login ok, tradingDay =", r.tradingDay);
    const rc = md.subscribe([cfg.symbol]);
    log("MD subscribed", cfg.symbol, "rc=", rc);
  } catch (e) {
    log("MD login FAILED:", e.message);
  }
});
md.on("front-disconnected", (reason) => log("MD front-disconnected", reason));
let tickCount = 0;
let lastSeenPrice = 0;
md.on("rtn-depth-market-data", (t) => {
  lastSeenPrice = t.lastPrice;
  if (tickCount++ < 5)
    log(`tick ${t.instrumentId} last=${t.lastPrice} bid=${t.bidPrice1} ask=${t.askPrice1} vol=${t.volume} @${t.updateTime}`);
});

// ---------------- Trader ----------------
const td = new Trader("./flow-td/", cfg.tdFront);
td.riskSet({ maxOrderVolume: 5, maxOrdersPerSec: 5 });
td.on("front-connected", async () => {
  log("TD front-connected");
  try {
    if (cfg.skipAuth) {
      log("TD skipping authenticate (CTP_SKIP_AUTH=1)");
    } else {
      try {
        await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode });
        log("TD authenticated");
      } catch (e) {
        log("TD authenticate failed (continuing to login):", e.message, "errorId=", e.errorId);
      }
    }
    const li = await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("TD login ok, frontId =", li.frontId, "sessionId =", li.sessionId, "maxOrderRef =", li.maxOrderRef);

    // Settlement confirm is required before trading queries on many brokers.
    await td.reqSettlementInfoConfirm({ brokerId: cfg.brokerId, investorId: cfg.userId }).catch(() => {});

    const acct = await td.reqQryTradingAccount({ brokerId: cfg.brokerId, investorId: cfg.userId });
    log("TD trading account rows:", acct.length, acct[0] ? `available=${acct[0].available} balance=${acct[0].balance}` : "");

    await sleep(1200); // SimNow throttles queries to 1/sec
    const pos = await td.reqQryInvestorPosition({ brokerId: cfg.brokerId, investorId: cfg.userId });
    log("TD positions rows:", pos.length, pos.map((p) => `${p.instrumentId}:${p.position}`).slice(0, 6).join(" "));

    if (cfg.placeOrder) {
      log("placing a test order: BUY 1 @ 1 (far below market -> rejected, no fill) ...");
      await sleep(1200);
      td.reqOrderInsert({
        brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: cfg.symbol,
        orderRef: "smoke1", direction: "0", combOffsetFlag: "0", combHedgeFlag: "1",
        orderPriceType: "2", limitPrice: 1, volumeTotalOriginal: 1,
        timeCondition: "3", volumeCondition: "1", contingentCondition: "1",
        forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0,
      })
        .then((o) => log("order rsp: accepted", JSON.stringify(o)))
        .catch((e) => log("order rsp: rejected -", e.message, "errorId=", e.errorId));
    }

    if (cfg.arm) {
      log("arming BUY 1 @ 1 (fires on the next tick in C++, rejected at exchange, no fill)...");
      td.arm(md, {
        instrumentId: cfg.symbol, side: "buy", triggerPrice: 99999,
        order: {
          brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: cfg.symbol,
          orderRef: "armfire1", direction: "0", combOffsetFlag: "0", combHedgeFlag: "1",
          orderPriceType: "2", limitPrice: 1, volumeTotalOriginal: 1,
          timeCondition: "3", volumeCondition: "1", contingentCondition: "1",
          forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0,
        },
      });
      setTimeout(() => log("arm fireCount =", td._armFireCount()), 4000);
    }

    if (cfg.devDemo) {
      td.riskSet({ maxPriceDeviation: 0.05 });
      td.trackMarketData(md);
      log("maxPriceDeviation 5% + trackMarketData(md); waiting for live ref price...");
      await sleep(2500);
      const bad = Math.round((lastSeenPrice || 3000) * 1.2); // +20% -> should be blocked
      const r = await td
        .reqOrderInsert({
          brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: cfg.symbol,
          orderRef: "dev1", direction: "0", combOffsetFlag: "0", combHedgeFlag: "1",
          orderPriceType: "2", limitPrice: bad, volumeTotalOriginal: 1,
          timeCondition: "3", volumeCondition: "1", contingentCondition: "1",
          forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0,
        })
        .then(() => "SENT (unexpected!)")
        .catch((e) => e.message);
      log(`deviation demo: BUY @ ${bad} vs live≈${lastSeenPrice} -> ${r}`);
    }

    if (cfg.riskLive) {
      log("--- live position-cost verification (auto multiplier + auto position) ---");
      const inst = await td.reqQryInstrument({ instrumentId: cfg.symbol });
      log(`reqQryInstrument ${cfg.symbol}: volumeMultiple=${inst[0]?.volumeMultiple}`);
      await sleep(1100);
      log(`syncMultipliers applied ${await td.syncMultipliers([cfg.symbol])}`);
      td.riskSet({ maxPositionCost: 100_000_000 }); // high: won't block, just track
      const buyPx = Math.round((lastSeenPrice || 3200) + 30);
      log(`opening BUY 1 @ ${buyPx} (marketable -> should fill) ...`);
      await td
        .reqOrderInsert({ brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: cfg.symbol, orderRef: "rlopen", direction: "0", combOffsetFlag: "0", combHedgeFlag: "1", orderPriceType: "2", limitPrice: buyPx, volumeTotalOriginal: 1, timeCondition: "3", volumeCondition: "1", contingentCondition: "1", forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0 })
        .then(() => log("open accepted")).catch((e) => log("open:", e.message));
      await sleep(2500);
      log(`positionCost after fill (auto from OnRtnTrade) = ${td.positionCost()}  (expect ~ ${buyPx}*10)`);
      await sleep(1100);
      log(`syncPositions seeded ${await td.syncPositions()} position(s); positionCost = ${td.positionCost()}`);
      await sleep(1100);
      const sellPx = Math.round((lastSeenPrice || 3100) - 30);
      log(`closing SELL 1 @ ${sellPx} (平今) to flatten ...`);
      await td
        .reqOrderInsert({ brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: cfg.symbol, orderRef: "rlclose", direction: "1", combOffsetFlag: "3", combHedgeFlag: "1", orderPriceType: "2", limitPrice: sellPx, volumeTotalOriginal: 1, timeCondition: "3", volumeCondition: "1", contingentCondition: "1", forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0 })
        .then(() => log("close accepted")).catch((e) => log("close:", e.message));
      await sleep(1500);
    }
  } catch (e) {
    log("TD flow FAILED:", e.message, "errorId=", e.errorId);
  }
});
td.on("front-disconnected", (reason) => log("TD front-disconnected", reason));
td.on("rtn-order", (o) => log("rtn-order", `ref=${o.orderRef} status=${o.orderStatus} msg=${o.statusMsg}`));
td.on("rtn-trade", (t) => log("rtn-trade", t.instrumentId, t.price, t.volume));
td.on("err-rtn-order-insert", (_d, o) => log("err-rtn-order-insert:", o.rspInfo?.errorMsg));

log("connecting... MD", cfg.mdFront, "| TD", cfg.tdFront, "| user", cfg.userId);
setTimeout(() => {
  log(`done. received ${tickCount} ticks.`);
  md.close();
  td.close();
  process.exit(0);
}, Number(env.CTP_RUN_MS ?? 20000));
