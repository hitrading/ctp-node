// Live SimNow integration smoke. Reads credentials from env so nothing secret
// is committed. Run, e.g.:
//   CTP_USER=xxxx CTP_PASS=yyyy node scripts/test-simnow.mjs
//
// Defaults target SimNow (brokerId 9999, the standard test AppID/AuthCode).
// Override any of: CTP_BROKER CTP_USER CTP_PASS CTP_APPID CTP_AUTHCODE
//                  CTP_MD_FRONT CTP_TD_FRONT CTP_SYMBOL CTP_PLACE_ORDER
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
  mdFront: env.CTP_MD_FRONT ?? "tcp://180.168.146.187:10211",
  tdFront: env.CTP_TD_FRONT ?? "tcp://180.168.146.187:10201",
  symbol: env.CTP_SYMBOL ?? "rb2610",
  placeOrder: env.CTP_PLACE_ORDER === "1",
};

if (!cfg.userId || !cfg.password) {
  console.error("Set CTP_USER and CTP_PASS (SimNow account). See header for all env vars.");
  process.exit(2);
}

mkdirSync("flow-md", { recursive: true });
mkdirSync("flow-td", { recursive: true });
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);

// ---------------- Market data ----------------
const md = new MarketData("./flow-md/", cfg.mdFront);
md.on("front-connected", async () => {
  log("MD front-connected");
  try {
    const r = await md.login({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("MD login ok, tradingDay =", r.tradingDay);
    md.subscribe([cfg.symbol]);
    log("MD subscribed", cfg.symbol);
  } catch (e) {
    log("MD login FAILED:", e.message);
  }
});
md.on("front-disconnected", (reason) => log("MD front-disconnected", reason));
let tickCount = 0;
md.on("rtn-depth-market-data", (t) => {
  if (tickCount++ < 5)
    log(`tick ${t.instrumentId} last=${t.lastPrice} bid=${t.bidPrice1} ask=${t.askPrice1} vol=${t.volume} @${t.updateTime}`);
});

// ---------------- Trader ----------------
const td = new Trader("./flow-td/", cfg.tdFront);
td.riskSet({ maxOrderVolume: 5, maxOrdersPerSec: 5 });
td.on("front-connected", async () => {
  log("TD front-connected");
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode });
    log("TD authenticated");
    const li = await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("TD login ok, frontId =", li.frontId, "sessionId =", li.sessionId, "maxOrderRef =", li.maxOrderRef);

    // Settlement confirm is required before trading queries on many brokers.
    await td.reqSettlementInfoConfirm({ brokerId: cfg.brokerId, investorId: cfg.userId }).catch(() => {});

    const acct = await td.reqQryTradingAccount({ brokerId: cfg.brokerId, investorId: cfg.userId });
    log("TD trading account rows:", acct.length, acct[0] ? `available=${acct[0].available}` : "");

    const pos = await td.reqQryInvestorPosition({ brokerId: cfg.brokerId, investorId: cfg.userId });
    log("TD positions rows:", pos.length);

    if (cfg.placeOrder) {
      log("placing a test order (CTP_PLACE_ORDER=1) ...");
      const rc = await td
        .reqOrderInsert({
          brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: cfg.symbol,
          orderRef: "smoke1", direction: "0", combOffsetFlag: "0", combHedgeFlag: "1",
          orderPriceType: "2", limitPrice: 1, volumeTotalOriginal: 1,
          timeCondition: "3", volumeCondition: "1", contingentCondition: "1",
          forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0,
        })
        .then(() => "accepted")
        .catch((e) => `rejected: ${e.message}`);
      log("test order:", rc, "(price 1 will be rejected by exchange — that's expected)");
    }
  } catch (e) {
    log("TD flow FAILED:", e.message);
  }
});
td.on("front-disconnected", (reason) => log("TD front-disconnected", reason));
td.on("rtn-order", (o) => log("rtn-order", o.orderRef, "status=", o.orderStatus, o.statusMsg));
td.on("rtn-trade", (t) => log("rtn-trade", t.instrumentId, t.price, t.volume));

log("connecting... MD", cfg.mdFront, "| TD", cfg.tdFront, "| user", cfg.userId);
setTimeout(() => {
  log(`done. received ${tickCount} ticks.`);
  md.close();
  td.close();
  process.exit(0);
}, Number(env.CTP_RUN_MS ?? 20000));
