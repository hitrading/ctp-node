// Full-market stress test (LIVE SimNow, trading hours).
//
// Goal: subscribe to EVERY instrument in the market, run a per-instrument
// indicator (SMA + cross signal) on every tick, and place/cancel real orders on
// the most-active future under that load — while measuring whether any of it
// blows up memory, pins the CPU, or makes the market-data feed back up
// (droppedRecords > 0 = the JS loop fell behind = "行情挤压").
//
// Credentials come from env (nothing secret is written to the file). Run, e.g.:
//   CTP_USER=xxxx CTP_PASS=yyyy node scripts/stress-fullmarket.mjs
// Env (all optional except USER/PASS):
//   CTP_BROKER CTP_USER CTP_PASS CTP_APPID CTP_AUTHCODE CTP_MD_FRONT CTP_TD_FRONT
//   CTP_RUN_MS (default 90000)  CTP_MA (SMA period, default 20)
//   CTP_TRADE (1=place/cancel test orders, default 1)  CTP_ORDER_SYMBOL (auto if unset)
import { MarketData, Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";
import { monitorEventLoopDelay } from "node:perf_hooks";
import process from "node:process";

const env = process.env;
const cfg = {
  brokerId: env.CTP_BROKER ?? "9999",
  userId: env.CTP_USER ?? "",
  password: env.CTP_PASS ?? "",
  appId: env.CTP_APPID ?? "simnow_client_test",
  authCode: env.CTP_AUTHCODE ?? "0000000000000000",
  // SimNow fronts rotate; override via env. These are the 上期技术-电信二 site.
  mdFront: env.CTP_MD_FRONT ?? "tcp://182.254.243.31:30012",
  tdFront: env.CTP_TD_FRONT ?? "tcp://182.254.243.31:30002",
  runMs: Number(env.CTP_RUN_MS ?? 90000),
  maPeriod: Number(env.CTP_MA ?? 20),
  trade: (env.CTP_TRADE ?? "1") === "1",
  orderSymbol: env.CTP_ORDER_SYMBOL ?? "",
};
if (!cfg.userId || !cfg.password) {
  console.error("Set CTP_USER and CTP_PASS (SimNow account).");
  process.exit(2);
}

const t0 = Date.now();
const ts = () => new Date().toISOString().slice(11, 23);
const log = (...a) => console.log(ts(), ...a);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
process.on("uncaughtException", (e) => log("!!! UNCAUGHT", e?.stack || e));
process.on("unhandledRejection", (e) => log("!!! UNHANDLED", e?.message || e));

mkdirSync("flow-md", { recursive: true });
mkdirSync("flow-td", { recursive: true });

// ---- event-loop lag monitor (the truest "are we keeping up?" metric) ----
const eld = monitorEventLoopDelay({ resolution: 10 });
eld.enable();

// ---- per-instrument strategy state (SMA over the last N lastPrices) ----
/** @type {Map<string, {buf:number[], i:number, sum:number, n:number, last:number, vol:number}>} */
const state = new Map();
let totalTicks = 0;
let signalCount = 0;
let strategyNanos = 0n;
let dropBaseline = 0;

function onTickStrategy(t) {
  const id = t.instrumentId;
  const px = t.lastPrice;
  // CTP sends DBL_MAX for an untraded price; skip those.
  if (!(px > 0) || px >= 1e300) return;
  let s = state.get(id);
  if (!s) { s = { buf: new Array(cfg.maPeriod).fill(0), i: 0, sum: 0, n: 0, last: 0, vol: 0 }; state.set(id, s); }
  // rolling SMA
  const old = s.buf[s.i];
  s.sum += px - old;
  s.buf[s.i] = px;
  s.i = (s.i + 1) % cfg.maPeriod;
  if (s.n < cfg.maPeriod) s.n++;
  s.vol = t.volume;
  if (s.n >= cfg.maPeriod) {
    const ma = s.sum / s.n;
    // simple cross signal: did price cross the MA since the last tick?
    const wasAbove = s.last >= ma;
    const isAbove = px >= ma;
    if (s.last > 0 && wasAbove !== isAbove) signalCount++; // a decision point
    s.last = px;
    return isAbove ? 1 : -1;
  }
  s.last = px;
  return 0;
}

// ================= Market data =================
const md = new MarketData("./flow-md/", cfg.mdFront);
md.on("error", (e) => log("[md handler error]", e?.message || e));
md.on("front-disconnected", (r) => log("MD front-disconnected", r));
let orderSymLastTick = null;
md.on("rtn-depth-market-data", (t) => {
  totalTicks++;
  const a = process.hrtime.bigint();
  onTickStrategy(t); // run the per-tick indicator on EVERY instrument (the load)
  strategyNanos += process.hrtime.bigint() - a;
  if (cfg.trade && t.instrumentId === orderSym) orderSymLastTick = t;
});

let allInstruments = [];
md.on("front-connected", async () => {
  log("MD connected; logging in...");
  try {
    await md.login({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("MD login ok. waiting for the instrument list from the trader...");
    await instrumentsReady;
    dropBaseline = md.droppedRecords;
    // subscribe EVERYTHING, in chunks (the end state — all subscribed — is the stress)
    const CHUNK = 1000;
    for (let i = 0; i < allInstruments.length; i += CHUNK) {
      const rc = md.subscribe(allInstruments.slice(i, i + CHUNK));
      if (rc !== 0) log("  subscribe chunk rc", rc, "at", i);
      await sleep(50);
    }
    log(`MD subscribed ALL ${allInstruments.length} instruments.`);
  } catch (e) { log("MD flow FAILED:", e.message); }
});

// ================= Trader =================
const td = new Trader("./flow-td/", cfg.tdFront);
td.riskSet({ maxOrderVolume: 2, maxOrdersPerSec: 5, maxPriceDeviation: 0.5 });
let orderSym = cfg.orderSymbol;
let orderTick = 0;     // price tick of the order symbol
let placed = 0, accepted = 0, cancelled = 0, rejected = 0;
let lastOrderAt = 0;
const working = new Map(); // orderRef -> {frontId, sessionId, instrumentId}

let resolveInstruments;
const instrumentsReady = new Promise((r) => { resolveInstruments = r; });

td.on("error", (e) => log("[td handler error]", e?.message || e));
td.on("front-disconnected", (r) => log("TD front-disconnected", r));
td.on("rtn-order", (o) => {
  if (o.instrumentId !== orderSym) return;
  if ((o.orderStatus === "3" || o.orderStatus === "1") && !working.has(o.orderRef)) {
    accepted++; // our resting order — cancel it ~1s later
    working.set(o.orderRef, { frontId: o.frontId, sessionId: o.sessionId, instrumentId: o.instrumentId });
    setTimeout(() => {
      const w = working.get(o.orderRef);
      if (w) td.reqOrderAction({ brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: w.instrumentId, orderRef: o.orderRef, frontId: w.frontId, sessionId: w.sessionId, actionFlag: "0" }).catch(() => {});
    }, 1000);
  } else if (o.orderStatus === "5") {
    if (working.delete(o.orderRef)) cancelled++;
  } else if (o.orderStatus === "0" || o.orderStatus === "2") {
    working.delete(o.orderRef);
  }
});
td.on("err-rtn-order-insert", (_d, o) => { rejected++; if (rejected <= 4) log("  order REJECTED(exch):", o.rspInfo?.errorMsg); });

td.on("front-connected", async () => {
  log("TD connected; authenticating...");
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode }).catch((e) => log("auth:", e.message));
    const li = await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    log("TD login ok, frontId", li.frontId, "sessionId", li.sessionId);
    await td.reqSettlementInfoConfirm({ brokerId: cfg.brokerId, investorId: cfg.userId }).catch(() => {});
    await sleep(1200);
    log("querying ALL instruments (one big multi-row response)...");
    const rows = await td.reqQryInstrument({}); // no filter -> every instrument
    // futures-like ids for subscribing + for picking an order symbol; subscribe ALL though
    allInstruments = rows.map((r) => r.instrumentId).filter(Boolean);
    log(`got ${allInstruments.length} instruments from the exchange.`);
    // auto-pick an order symbol: highest priceTick-having future (short id, no option markers)
    if (!orderSym) {
      const fut = rows.filter((r) => /^[a-zA-Z]{1,2}\d{3,4}$/.test(r.instrumentId) && r.priceTick > 0);
      // prefer a common liquid product family if present
      const pick = fut.find((r) => /^(rb|ru|hc|fu|bu|m|y|p|a|c|i|j|jm|cu|al|zn|TA|MA|FG|SA)\d/i.test(r.instrumentId)) || fut[0];
      if (pick) { orderSym = pick.instrumentId; orderTick = pick.priceTick; }
    } else {
      const r = rows.find((x) => x.instrumentId === orderSym);
      orderTick = r?.priceTick || 1;
    }
    log("order symbol for the place/cancel stress:", orderSym || "(none — futures not found)", "tick", orderTick);
    resolveInstruments();
  } catch (e) { log("TD flow FAILED:", e.message, "errorId", e.errorId); resolveInstruments(); }
});

// place a far-below-market resting BUY, then cancel it shortly after — exercises
// the insert+cancel round trip under the full-market tick load, no fills.
function maybeTrade(t) {
  if (!orderSym || !t) return;
  const bid = t.bidPrice1;
  if (!(bid > 0) || bid >= 1e300) return;
  lastOrderAt = Date.now();
  // a few ticks below the bid: rests behind the book (won't fill), well within
  // daily limits, always cancellable. NO explicit orderRef -> the Trader
  // auto-assigns a unique numeric ref (passing our own non-numeric/duplicate ref
  // is what CTP rejects with "不允许重复报单"). The cancel is driven from rtn-order.
  const px = Math.max(orderTick, bid - 3 * orderTick);
  placed++;
  td.reqOrderInsert({
    brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: orderSym,
    direction: "0", combOffsetFlag: "0", combHedgeFlag: "1",
    orderPriceType: "2", limitPrice: px, volumeTotalOriginal: 1,
    timeCondition: "3", volumeCondition: "1", contingentCondition: "1",
    forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0,
  }).catch((e) => { rejected++; if (rejected <= 4) log("order refused:", e.message); });
}

// ================= periodic stats =================
let lastCpu = process.cpuUsage();
let lastTicks = 0, lastT = Date.now();
let peakRss = 0;
const stat = setInterval(() => {
  const mem = process.memoryUsage();
  const rss = Math.round(mem.rss / 1048576);
  const heap = Math.round(mem.heapUsed / 1048576);
  peakRss = Math.max(peakRss, rss);
  const now = Date.now();
  const dt = (now - lastT) / 1000;
  const tps = Math.round((totalTicks - lastTicks) / dt);
  lastTicks = totalTicks; lastT = now;
  const cpu = process.cpuUsage(lastCpu); lastCpu = process.cpuUsage();
  const cpuPct = Math.round(((cpu.user + cpu.system) / 1000) / (dt * 1000) * 100);
  const dropped = md.droppedRecords - dropBaseline;
  const p50 = (eld.percentile(50) / 1e6).toFixed(1);
  const p99 = (eld.percentile(99) / 1e6).toFixed(1);
  const maxlag = (eld.max / 1e6).toFixed(0);
  const avgStratNs = totalTicks ? Number(strategyNanos / BigInt(totalTicks)) : 0;
  eld.reset();
  log(
    `t+${Math.round((now - t0) / 1000)}s | inst ticking ${state.size}/${allInstruments.length} | ` +
    `ticks ${totalTicks} (${tps}/s) | DROPPED ${dropped} | RSS ${rss}MB heap ${heap}MB | ` +
    `cpu ${cpuPct}% | loop-lag p50 ${p50} p99 ${p99} max ${maxlag} ms | ` +
    `strat ${avgStratNs}ns/tick sig ${signalCount} | orders p${placed}/a${accepted}/c${cancelled}/r${rejected}`
  );
}, 5000);

// Fire a place+cancel round-trip on a timer (independent of strategy signals) so
// the trade path is exercised under the full-market tick load even in a quiet
// session. The per-tick strategy compute above is the real CPU/backpressure load.
const orderTimer = cfg.trade ? setInterval(() => maybeTrade(orderSymLastTick), Number(env.CTP_ORDER_MS ?? 3000)) : null;

// ================= teardown + verdict =================
setTimeout(async () => {
  clearInterval(stat);
  if (orderTimer) clearInterval(orderTimer);
  await sleep(200);
  const dropped = md.droppedRecords - dropBaseline;
  const mem = process.memoryUsage();
  log("================= SUMMARY =================");
  log(`run ${Math.round((Date.now() - t0) / 1000)}s | subscribed ${allInstruments.length} | instruments that ticked ${state.size}`);
  log(`total ticks ${totalTicks} | DROPPED ${dropped} (${dropped === 0 ? "OK: feed never backed up" : "feed fell behind"})`);
  log(`avg strategy compute ${totalTicks ? Number(strategyNanos / BigInt(totalTicks)) : 0} ns/tick | signals ${signalCount}`);
  log(`orders placed ${placed} accepted ${accepted} cancelled ${cancelled} rejected ${rejected}`);
  log(`peak RSS ${peakRss}MB | final RSS ${Math.round(mem.rss / 1048576)}MB heap ${Math.round(mem.heapUsed / 1048576)}MB`);
  log(`loop-lag final p99 ${(eld.percentile(99) / 1e6).toFixed(1)}ms max ${(eld.max / 1e6).toFixed(0)}ms`);
  md.close(); td.close();
  await sleep(300);
  process.exit(0);
}, cfg.runMs);

log("connecting... MD", cfg.mdFront, "| TD", cfg.tdFront, "| run", cfg.runMs + "ms"); // never log the userId
