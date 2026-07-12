// Broad interface + boundary verification on SimNow. Queries are market-
// independent; boundary orders are risk-blocked (no send) or below-limit
// (exchange-rejected), so nothing fills - the account is untouched. Each probe
// is logged pass/fail; queries are best-effort (logged, never abort the run).
import { MarketData, Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";
import process from "node:process";

const e = process.env;
const C = {
  brokerId: e.CTP_BROKER ?? "9999", userId: e.CTP_USER, password: e.CTP_PASS,
  appId: "simnow_client_test", authCode: "0000000000000000",
  md: e.CTP_MD_FRONT, td: e.CTP_TD_FRONT,
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
let pass = 0, fail = 0;
const check = (c, m) => (c ? (pass++, log("  PASS:", m)) : (fail++, log("  ** FAIL:", m)));
["flow-md", "flow-td"].forEach((d) => mkdirSync(d, { recursive: true }));
const ob = { brokerId: C.brokerId, investorId: C.userId, combHedgeFlag: "1", orderPriceType: "2", timeCondition: "3", volumeCondition: "1", contingentCondition: "1", forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0 };

// ---- MarketData interface ----
const md = new MarketData("./flow-md/", C.md);
let ticks = 0;
md.on("front-connected", async () => {
  await md.login({ brokerId: C.brokerId, userId: C.userId, password: C.password }).catch(() => {});
  md.subscribe(["au2608", "ag2608"]);
});
md.on("rtn-depth-market-data", () => ticks++);

const td = new Trader("./flow-td/", C.td);
const tryQ = async (label, fn, ok) => {
  try { const r = await fn(); check(ok ? ok(r) : true, `${label}: ${Array.isArray(r) ? r.length + " rows" : "ok"}`); return r; }
  catch (err) { check(false, `${label}: threw ${err.message} (errorId=${err.errorId})`); return undefined; }
};
const sendOutcome = async (o) => {
  try { await td.reqOrderInsert(o); return "SENT"; }
  catch (x) { return x?.message ?? String(x); }
};

td.on("front-connected", async () => {
  try {
    // ---- session() handshake ----
    const s = await td.session({ brokerId: C.brokerId, userId: C.userId, password: C.password, appId: C.appId, authCode: C.authCode, confirmSettlement: true, sync: { multipliers: ["au2608"], positions: true, orders: true } });
    check(s && typeof s.multipliers === "number", `session() -> ${JSON.stringify(s)}`);
    check(td.apiVersion.length > 0, `apiVersion -> ${td.apiVersion}`);
    check(/^\d{8}$/.test(td.tradingDay), `tradingDay -> ${td.tradingDay}`);
    check(td.droppedRecords === 0, `droppedRecords -> ${td.droppedRecords}`);

    // ---- query interfaces (market-independent) ----
    log("== queries ==");
    await tryQ("reqQryTradingAccount", () => td.reqQryTradingAccount({ brokerId: C.brokerId, investorId: C.userId }));
    await tryQ("reqQryInstrument(au2608)", () => td.reqQryInstrument({ instrumentId: "au2608" }), (r) => r.some((x) => x.instrumentId === "au2608" && x.volumeMultiple === 1000));
    await tryQ("reqQryInvestorPosition", () => td.reqQryInvestorPosition({ brokerId: C.brokerId, investorId: C.userId }));
    await tryQ("reqQryInvestorPositionDetail", () => td.reqQryInvestorPositionDetail({ brokerId: C.brokerId, investorId: C.userId }));
    await tryQ("reqQryOrder", () => td.reqQryOrder({ brokerId: C.brokerId, investorId: C.userId }));
    await tryQ("reqQryTrade", () => td.reqQryTrade({ brokerId: C.brokerId, investorId: C.userId }));
    await tryQ("reqQryInstrumentMarginRate", () => td.reqQryInstrumentMarginRate({ brokerId: C.brokerId, investorId: C.userId, instrumentId: "au2608" }));
    await tryQ("reqQryInstrumentCommissionRate", () => td.reqQryInstrumentCommissionRate({ brokerId: C.brokerId, investorId: C.userId, instrumentId: "au2608" }));
    await tryQ("reqQrySettlementInfoConfirm", () => td.reqQrySettlementInfoConfirm({ brokerId: C.brokerId, investorId: C.userId }));

    // ---- auto-sourced risk inputs (fed into the C++ engine directly from the SPI
    // responses OnRspQryInstrument / OnRspQryInstrumentMarginRate - no JS feed) ----
    log("== auto-sourced risk inputs ==");
    td.trackMarketData(md); // monetary gates require the same fresh C++ reference
    // Multiplier proof: au2608 x1000 reached the engine. A maxNotional that only
    // trips WHEN the multiplier is applied - 700*1*1000 = 700k > 200k -> blocked;
    // with the 1.0 default it would be 700 < 200k -> pass. check() blocks client-
    // side (no send): a clean, deterministic proof the multiplier auto-sourced.
    td.setRiskLimits({ maxOrderVolume: 0, maxNotional: 200000, maxAccountMargin: 0 });
    check(/maxNotional/i.test(await sendOutcome({ ...ob, instrumentId: "au2608", direction: "0", combOffsetFlag: "0", limitPrice: 700, volumeTotalOriginal: 1 })),
      "auto multiplier: au2608 x1000 sourced from OnRspQryInstrument (700*1*1000 > 200k notional -> blocked)");
    td.setRiskLimits({ maxNotional: 0 }); // clear the probe cap

    // ---- boundary conditions on the order gate (no fills) ----
    log("== boundaries ==");
    td.setRiskLimits({ maxOrderVolume: 5 });
    check(/positive int32|volume must be positive/i.test(await sendOutcome({ ...ob, instrumentId: "au2608", direction: "0", combOffsetFlag: "0", limitPrice: 700, volumeTotalOriginal: 0 })), "order volume 0 -> rejected with reason");
    check(/maxOrderVolume/i.test(await sendOutcome({ ...ob, instrumentId: "au2608", direction: "0", combOffsetFlag: "0", limitPrice: 700, volumeTotalOriginal: 100 })), "order volume 100 > 5 -> rejected with reason");
    td.reduceOnly();
    check(/reduce-only|pre-trade risk/i.test(await sendOutcome({ ...ob, instrumentId: "au2608", direction: "0", combOffsetFlag: "0", limitPrice: 700, volumeTotalOriginal: 1 })), "reduce-only -> rejected with reason");
    td.resume();
    check(!/pre-trade risk/i.test(await sendOutcome({ ...ob, instrumentId: "au2608", direction: "0", combOffsetFlag: "0", limitPrice: 700, volumeTotalOriginal: 1 })), "after resume -> passes risk gate (below-limit, exchange-rejected)");
    // cancel a bogus order -> CTP error surfaces (not a crash)
    const cancel = await td.reqOrderAction({ brokerId: C.brokerId, investorId: C.userId, instrumentId: "au2608", exchangeId: "SHFE", orderSysId: "000000000", actionFlag: "0" }).then(() => "sent").catch((x) => x.message);
    check(typeof cancel === "string", `reqOrderAction(bogus) handled -> ${cancel.slice(0, 60)}`);

    // ---- MD subscribe/unsubscribe + ticks flowing ----
    log("== market data ==");
    check(ticks > 0, `md ticks flowing -> ${ticks} received`);

    // ---- last-value cache (LVC) + deviation reference read in C++ ----
    log("== snapshot cache (LVC) ==");
    check(md.last("au2608") > 0, `md.last(au2608) from the C++ LVC -> ${md.last("au2608")}`);
    const lvc = md.snapshot("au2608");
    check(lvc !== null && lvc.instrumentId === "au2608", `md.snapshot(au2608) -> last ${lvc && lvc.lastPrice}, bid ${lvc && lvc.bidPrice1}`);
    // trackMarketData wires the MD snapshot into the risk engine; the deviation
    // check then reads au's live ref price in C++ (no JS). An order far from it
    // must be blocked with a deviation reason - proving the C++ read end-to-end.
    td.trackMarketData(md);
    td.setRiskLimits({ maxOrderVolume: 0, maxNotional: 0, maxPriceDeviation: 0.05 });
    const devOut = await sendOutcome({ ...ob, instrumentId: "au2608", direction: "0", combOffsetFlag: "0", limitPrice: 100, volumeTotalOriginal: 1 });
    check(/deviat/i.test(devOut), `auto ref price: au2608 @100 vs live ~${Math.round(md.last("au2608"))} (MD snapshot) -> deviation blocked`);
    td.setRiskLimits({ maxPriceDeviation: 0 }); // clear

    md.unsubscribe(["ag2608"]);
    check(true, "md.unsubscribe(ag2608) did not throw");
    check(md.snapshot("ag2608") === null, "LVC entry cleared on unsubscribe(ag2608)");
    md.subscribe(["cu2608"]);
    check(true, "md.subscribe(cu2608) did not throw");

    // ---- teardown idempotency ----
    log("== teardown ==");
    md.close(); md.close();
    td.close(); td.close();
    check(true, "double close() on md+td did not crash");

    log(`\nRESULT: ${pass} pass, ${fail} fail`);
  } catch (err) { fail++; log("FATAL:", err.message, "errorId=", err.errorId); }
  finally { try { md.close(); td.close(); } catch {} process.exit(fail ? 1 : 0); }
});

log("connecting...");
setTimeout(() => { try { md.close(); td.close(); } catch {} process.exit(1); }, Number(e.CTP_RUN_MS ?? 40000));
