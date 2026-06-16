// strategy.mjs - LIVE SimNow intraday strategy over the FULL market.
//
// Subscribes EVERY instrument, builds 1-minute K-lines per contract, and computes
// MA5 / MA10 (5- and 10-period SMA of the 1-min closes) + MACD(12,26,9). Signals:
//   - MA5 crosses UP through MA10 (golden cross) -> BUY  (open 1 long)
//   - MACD dead cross (DIF crosses below DEA)     -> SELL (close that long)
// Long-only, signal-driven. SimNow = a SIMULATION account (no real money).
//
// SAFETY (so "all contracts" can't run away): every order passes the C++ pre-trade
// risk gate (max order volume, orders/sec, account-wide open-cost cap); on top of
// that the JS side caps concurrent open positions (STRAT_MAX_POS) and never opens a
// contract it is already long (dedup). Stop any time with Ctrl-C (kill-switch) or
// by the scheduled stop time.
//
// CREDENTIALS come ONLY from the environment - never written to a file or the task:
//   CTP_BROKER CTP_USER CTP_PASS CTP_APPID CTP_AUTHCODE CTP_MD_FRONT CTP_TD_FRONT
// Tunables (env, optional):
//   STRAT_LOTS (1)  STRAT_MAX_POS (30)  STRAT_MAX_COST (5000000)
//   STRAT_MAX_OPS (5 = max order sends/sec)  STRAT_STOP (15:05 = HH:MM local exit)
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
  mdFront: env.CTP_MD_FRONT ?? "tcp://182.254.243.31:30012",
  tdFront: env.CTP_TD_FRONT ?? "tcp://182.254.243.31:30002",
  lots: Number(env.STRAT_LOTS ?? 1),
  maxPos: Number(env.STRAT_MAX_POS ?? 30),
  maxCost: Number(env.STRAT_MAX_COST ?? 5_000_000),
  maxOps: Number(env.STRAT_MAX_OPS ?? 5),
  stop: env.STRAT_STOP ?? "15:05",
};
if (!cfg.userId || !cfg.password) { console.error("Set CTP_USER and CTP_PASS (SimNow account)."); process.exit(2); }

const ts = () => new Date().toISOString().slice(11, 23);
const log = (...a) => console.log(ts(), ...a);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
process.on("uncaughtException", (e) => log("!!! UNCAUGHT", e?.stack || e));
process.on("unhandledRejection", (e) => log("!!! UNHANDLED", e?.message || e));
mkdirSync("flow-md", { recursive: true });
mkdirSync("flow-td", { recursive: true });

// ---------- per-contract K-line + indicator state ----------
const newState = () => ({
  bucket: "", close: 0,                      // current forming 1-min bar (close = last px this minute)
  closes: [], sum5: 0, sum10: 0,             // finalized closes + rolling MA sums
  ema12: 0, ema26: 0, dea: 0, n: 0,          // MACD EMAs; n = finalized bar count
  pMa5: 0, pMa10: 0, pDif: 0, pDea: 0, prev: false,
  bid: 0, ask: 0, px: 0, tick: 1, exch: "",  // latest quote + meta (for marketable orders)
});
const st = new Map();        // instrumentId -> state
const meta = new Map();      // instrumentId -> { tick, exch }
const longPos = new Map();   // instrumentId -> { lots }  (positions WE opened)
let allInstruments = [];
let totalTicks = 0, bars = 0, golden = 0, dead = 0, buys = 0, sells = 0, fills = 0, blocked = 0, dropBaseline = 0;
let stratNanos = 0n;

const order = (id, dir, off, price, vol) => ({
  brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: id,
  direction: dir, combOffsetFlag: off, combHedgeFlag: "1",
  orderPriceType: "2", limitPrice: price, volumeTotalOriginal: vol,
  timeCondition: "3", volumeCondition: "1", contingentCondition: "1",
  forceCloseReason: "0", isAutoSuspend: 0, userForceClose: 0,
});

function tryBuy(id, s) {
  if (longPos.has(id)) return;                              // already long (dedup)
  if (longPos.size >= cfg.maxPos) { blocked++; return; }    // concurrency cap
  const price = s.ask > 0 && s.ask < 1e300 ? s.ask : s.px;  // marketable (hit the ask)
  if (!(price > 0)) return;
  longPos.set(id, { lots: cfg.lots });                      // optimistic; rolled back if the send is refused
  buys++;
  log(`GOLDEN ${id} -> BUY ${cfg.lots} @ ${price} (open ${longPos.size}/${cfg.maxPos})`);
  td.reqOrderInsert(order(id, "0", "0", price, cfg.lots)).catch((e) => {
    longPos.delete(id); blocked++;
    if (blocked <= 10) log("  BUY refused", id, e.message);
  });
}
function trySell(id, s) {
  const p = longPos.get(id);
  if (!p) return;                                            // not our long -> nothing to close
  const price = s.bid > 0 && s.bid < 1e300 ? s.bid : s.px;   // marketable (hit the bid)
  if (!(price > 0)) return;
  const off = s.exch === "SHFE" || s.exch === "INE" ? "3" : "1"; // close-today (intraday)
  longPos.delete(id);
  sells++;
  log(`MACD-DEAD ${id} -> SELL(close) ${p.lots} @ ${price}`);
  td.reqOrderInsert(order(id, "1", off, price, p.lots)).catch((e) => {
    longPos.set(id, p);                                      // restore if the close send was refused
    if (sells <= 10) log("  SELL refused", id, e.message);
  });
}

// Finalize the just-ended 1-min bar: push its close, roll MA5/MA10, step MACD EMAs,
// detect a golden cross (MA) / dead cross (MACD) vs the previous bar, and act.
function finalizeBar(id, s) {
  const c = s.close;
  s.closes.push(c); bars++; s.n++;
  s.sum5 += c; s.sum10 += c;
  if (s.closes.length > 5) s.sum5 -= s.closes[s.closes.length - 6];
  if (s.closes.length > 10) s.sum10 -= s.closes[s.closes.length - 11];
  if (s.n === 1) { s.ema12 = c; s.ema26 = c; }
  else { s.ema12 += (2 / 13) * (c - s.ema12); s.ema26 += (2 / 27) * (c - s.ema26); }
  const dif = s.ema12 - s.ema26;
  s.dea = s.n === 1 ? dif : s.dea + (2 / 10) * (dif - s.dea);
  const ma5 = s.sum5 / Math.min(5, s.closes.length);
  const ma10 = s.sum10 / Math.min(10, s.closes.length);
  if (s.prev) {
    // golden cross needs both this & prev bar to have a full MA10 (n >= 11)
    if (s.n >= 11 && s.pMa5 <= s.pMa10 && ma5 > ma10) { golden++; tryBuy(id, s); }
    // MACD dead cross only after the EMAs/DEA have warmed up (~35 bars)
    if (s.n >= 35 && s.pDif >= s.pDea && dif < s.dea) { dead++; trySell(id, s); }
  }
  s.pMa5 = ma5; s.pMa10 = ma10; s.pDif = dif; s.pDea = s.dea; s.prev = true;
  if (s.closes.length > 64) s.closes.shift(); // cap memory (MACD needs ~35; MA needs 10)
}

// ================= Market data =================
const md = new MarketData("./flow-md/", cfg.mdFront);
md.on("error", (e) => log("[md handler error]", e?.message || e));
md.on("front-disconnected", (r) => log("MD front-disconnected", r));
md.on("rtn-depth-market-data", (t) => {
  totalTicks++;
  const a = process.hrtime.bigint();
  const id = t.instrumentId;
  const px = t.lastPrice;
  if (px > 0 && px < 1e300) {                                // skip DBL_MAX / no-trade prints
    let s = st.get(id);
    if (!s) { s = newState(); const m = meta.get(id); if (m) { s.tick = m.tick; s.exch = m.exch; } st.set(id, s); }
    s.px = px;
    if (t.bidPrice1 > 0 && t.bidPrice1 < 1e300) s.bid = t.bidPrice1;
    if (t.askPrice1 > 0 && t.askPrice1 < 1e300) s.ask = t.askPrice1;
    const bucket = (t.updateTime || "").slice(0, 5);         // "HH:MM" exchange minute
    if (!bucket) { s.close = px; }
    else if (s.bucket === "") { s.bucket = bucket; s.close = px; }
    else { if (bucket !== s.bucket) { finalizeBar(id, s); s.bucket = bucket; } s.close = px; }
  }
  stratNanos += process.hrtime.bigint() - a;
});

md.on("front-connected", async () => {
  log("MD connected; logging in...");
  try {
    await md.login({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    await instrumentsReady;                                  // wait for the trader's instrument list
    dropBaseline = md.droppedRecords;
    const CHUNK = 1000;
    for (let i = 0; i < allInstruments.length; i += CHUNK) {
      md.subscribe(allInstruments.slice(i, i + CHUNK));
      await sleep(50);
    }
    log(`MD subscribed ALL ${allInstruments.length} instruments.`);
  } catch (e) { log("MD flow FAILED:", e.message); }
});

// ================= Trader =================
const td = new Trader("./flow-td/", cfg.tdFront);
td.riskSet({ maxOrderVolume: cfg.lots, maxOrdersPerSec: cfg.maxOps, maxPositionCost: cfg.maxCost });
td.on("error", (e) => log("[td handler error]", e?.message || e));
td.on("front-disconnected", (r) => log("TD front-disconnected", r));
td.on("rtn-trade", (tr) => { fills++; log(`  FILL ${tr.instrumentId} ${tr.offsetFlag === "0" ? "OPEN" : "CLOSE"} ${tr.volume}@${tr.price}`); });
td.on("err-rtn-order-insert", (_d, o) => { if (blocked <= 10) log("  exch-reject", o.rspInfo?.errorMsg); });

let resolveInstruments;
const instrumentsReady = new Promise((r) => { resolveInstruments = r; });
let handshakeDone = false;
td.on("front-connected", async () => {
  log("TD connected; handshake...");
  try {
    await td.reqAuthenticate({ brokerId: cfg.brokerId, userId: cfg.userId, appId: cfg.appId, authCode: cfg.authCode }).catch(() => {});
    await td.reqUserLogin({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password });
    await td.reqSettlementInfoConfirm({ brokerId: cfg.brokerId, investorId: cfg.userId }).catch(() => {});
    if (!handshakeDone) {
      await sleep(1200);
      const rows = await td.reqQryInstrument({});            // one big multi-row response = every instrument
      allInstruments = rows.map((r) => r.instrumentId).filter(Boolean);
      for (const r of rows) {
        if (!r.instrumentId) continue;
        meta.set(r.instrumentId, { tick: r.priceTick || 1, exch: r.exchangeId || "" });
        if (r.volumeMultiple > 0) td.setMultiplier(r.instrumentId, r.volumeMultiple); // seed the cost cap
      }
      log(`got ${allInstruments.length} instruments; seeding existing positions...`);
      await sleep(1100);
      await td.syncPositions().catch(() => {});              // seed the risk engine with any existing position cost
      handshakeDone = true;
      resolveInstruments();
    }
    log("TD handshake ok.");
  } catch (e) { log("TD flow FAILED:", e.message, "errorId", e.errorId); resolveInstruments(); }
});

// ================= periodic stats =================
const eld = monitorEventLoopDelay({ resolution: 10 });
eld.enable();
const t0 = Date.now();
let lastCpu = process.cpuUsage(), lastTicks = 0, lastT = Date.now(), peakRss = 0;
const [sh, sm] = cfg.stop.split(":").map(Number);
const stat = setInterval(() => {
  const now = new Date();
  const rss = Math.round(process.memoryUsage().rss / 1048576); peakRss = Math.max(peakRss, rss);
  const dt = (Date.now() - lastT) / 1000;
  const tps = Math.round((totalTicks - lastTicks) / dt); lastTicks = totalTicks; lastT = Date.now();
  const cpu = process.cpuUsage(lastCpu); lastCpu = process.cpuUsage();
  const cpuPct = Math.round((cpu.user + cpu.system) / 1000 / (dt * 1000) * 100);
  const p99 = (eld.percentile(99) / 1e6).toFixed(1); eld.reset();
  log(`t+${Math.round((Date.now() - t0) / 1000)}s | ticking ${st.size}/${allInstruments.length} | ticks ${totalTicks} (${tps}/s) | DROPPED ${md.droppedRecords - dropBaseline} | bars ${bars} | golden ${golden} dead ${dead} | BUY ${buys} SELL ${sells} fills ${fills} blocked ${blocked} | open ${longPos.size}/${cfg.maxPos} | RSS ${rss}MB cpu ${cpuPct}% lag-p99 ${p99}ms`);
  if (now.getHours() > sh || (now.getHours() === sh && now.getMinutes() >= sm)) { log(`stop time ${cfg.stop} reached.`); shutdown(0); }
}, 5000);

async function shutdown(code) {
  clearInterval(stat);
  log("================= SUMMARY =================");
  log(`subscribed ${allInstruments.length} | bars ${bars} | golden ${golden} dead ${dead} | BUY ${buys} SELL ${sells} fills ${fills} blocked ${blocked} | still-open ${longPos.size} | peakRSS ${peakRss}MB | dropped ${md.droppedRecords - dropBaseline}`);
  if (longPos.size) log(`NOTE: ${longPos.size} position(s) still open at exit: ${[...longPos.keys()].slice(0, 20).join(",")}${longPos.size > 20 ? "..." : ""}`);
  try { md.close(); td.close(); } catch { /* */ }
  await sleep(400); process.exit(code);
}
process.on("SIGINT", () => { log("SIGINT -> kill-switch (halt) + shutdown"); try { td.halt(); } catch { /* */ } shutdown(0); });
process.on("SIGTERM", () => shutdown(0));

log(`strategy starting | MD ${cfg.mdFront} | TD ${cfg.tdFront} | lots ${cfg.lots} maxPos ${cfg.maxPos} maxCost ${cfg.maxCost} ops/s ${cfg.maxOps} | stop ${cfg.stop}`);
