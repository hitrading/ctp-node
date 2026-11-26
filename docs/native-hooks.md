<!-- LANG-SWITCH -->
**English** · [简体中文](native-hooks.zh-CN.md)

# Native-sinkable hooks (risk / rate-limit / market-data snapshot)

Goal: keep ~95% of strategy work in JS/TS (fast iteration, good ecosystem) while
giving the **safety-critical** slices C++-grade determinism — no GC pause or
event-loop hiccup can defeat them. JS sets the rules; C++ enforces them on the
order path.

## Two worlds

| | C++ world | JS world |
|---|---|---|
| Runs on | CTP callback thread | Node main thread (event loop) |
| GC pauses | none | yes (occasional ms-tens-of-ms) |
| Determinism | microsecond, predictable | jitter (GC / JIT / scheduling) |

A tick first arrives on the **C++ callback thread**; a pure-JS check must cross
to JS and wait for the event loop. "Sinking down" keeps the last-line checks in
C++, right before the vendor send call; JS only configures them and is told why
an order was refused.

## What is sunk down

1. **Risk (pre-trade, stateless)** — `src/native/risk.h` `RiskEngine`
   Hard checks run before every order insert: the kill-switch modes
   (`REDUCE_ONLY` / `HALT_ALL`), max order volume, max single-order notional
   (fat-finger guard) and the price-deviation guard. Enforced in C++ so they hold
   even if the JS process is mid-GC, blocked, or buggy. *Safety win — worth it
   even for slow strategies.*
   - **Stateless by design.** Every rule judges one order from the order itself,
     the latest market reference and static instrument metadata. Nothing depends
     on what the account holds or has working, so there is no position/order
     ledger to rebuild after a reconnect and no state that can disagree with the
     caller's books. Volume, deviation and notional checks apply to opening
     orders; a closing order is refused only in `HALT_ALL` (or by the close-order
     rate limiter), so exits keep working when a quote or metadata is missing.
   - **What it deliberately does not do: account-level caps.** Account margin,
     per-instrument margin and position-lot caps need the account's positions,
     working orders and funds. CTP's position snapshot and its realtime
     order/trade stream share no common sequence number, so a ledger inside the
     binding could only mark itself uncertain after closes, refuse openings and
     guess at empty snapshots from a funds cross-check — duplicating the caller's
     own books and able to disagree with them. Those caps belong to the trading
     layer that owns the account facts (`reqQryInvestorPosition`, `reqQryOrder`,
     `reqQryTradingAccount`, `rtn-order` / `rtn-trade`); `syncMarginRates()` /
     `readMarginRates()` resolve margin schedules for its own calculations.
   - The price-deviation and notional checks read their reference from the C++
     MD snapshot cache (the LVC; see §3 below) via `trackMarketData(md)` — no
     `setRefPrice`, no JS round-trip. A missing quote, or one older than
     `maxReferenceAgeMs`, fails an enabled check closed for opening orders. The
     notional check uses `max(order price, reference) × volume × multiplier` and
     is enabled only for Futures/TAS product classes; other classes fail closed
     while it is on.
   - **Data sourcing: metadata and the session identity flow directly from SPI
     callbacks into C++.** Special-cased in `scripts/codegen/generate-trader.mjs`
     (emitted into `traderspi.gen.cc` / `traderreq.gen.cc`):
     - contract multipliers and product classes ← `OnRspQryInstrument`
       (internal `setInstrument`); sending a `reqQryInstrument` first
       invalidates the queried contract (or all contracts), so a failed or empty
       refresh cannot leave stale metadata active
     - login session (FrontID/SessionID/BrokerID) ← `OnRspUserLogin`, correlated
       with the request id of the latest login request; `session()` then confirms
       the trading account (InvestorID) before any ordinary order insert is
       accepted. Front connect/disconnect and every login/logout request revoke
       the confirmation.

     So any `reqQryInstrument` keeps multipliers current — there is no JS
     metadata relay. Margin schedules are not fed to the native gate.
   - **Unsupported requests.** Advanced mutations (quote/exec/comb/parked
     inserts, order modify, offset setting, spread/hedge confirmation) and
     bank/futures transfers cannot be judged by these per-order rules. They pass
     only in `NORMAL`, for a confirmed session, with no local control configured,
     and leave no state behind. Every non-query SDK request must be classified
     by the generator; an unreviewed method fails code generation.

2. **Rate limit** — `src/native/risk.h` `RateLimiter`
   Token buckets — one for opening inserts, an independent one for closing
   inserts — guaranteeing the seat never exceeds the configured order rate,
   immune to JS bursts after a pause.
   - Status: **real** (token bucket).

3. **Market-data snapshot (last-value cache)** — `src/native/snapshot.h`
   `SnapshotCache`
   A C++ last-value cache (LVC) holding the latest full `DepthMarketDataField`
   per instrument. A successful MD login activates a new empty epoch; only
   later ticks in that authenticated epoch update it before publication.
   Connect/disconnect, login/logout transitions and close invalidate it, so a
   prior-session quote cannot become a fresh risk reference after reconnect.
   Unsubscribe erases its instrument entry.
   - **JS reads it synchronously**: `md.snapshot(id)` returns the full latest
     tick (or `null` outside the active epoch / if none seen / unsubscribed), `md.last(id)` returns just
     the last price — no waiting for a tick, no per-tick bookkeeping in your
     strategy.
   - **The reference flows MD→risk entirely in C++.** The Trader's
     price-deviation and notional checks read the cache in C++ via
     `trackMarketData(md)`: the MarketData shares the cache as a `shared_ptr`,
     so the risk engine reads live prices straight from the MD feed with no JS
     round-trip. Unauthenticated or post-disconnect ticks remain observable
     protocol events but cannot update the cache. (The `SnapshotCache`
     implements `RefPriceSource`; the risk engine holds it via `setMdSnapshot`.)

## Threading / safety model

- Config is written on the **slow path** (JS thread) and read on the **hot
  path** (the order send). Risk config uses **atomics** (lock-free
  reads); the rate limiter uses a tiny mutex (bounded, uncontended).
- Configuration, mode changes, session transitions and every order insert share
  one send barrier, so a limit/mode/session change is linearized with in-flight
  sends: no order can pass the checks under the old state and then be sent after
  the change returns, or go out under a newly logged-in account.
- Queue/state-producing test hooks (`_injectTestTick`, `_injectTestForQuote`,
  `_injectTestLifecycle`, `_burstTicks`, `_produceTestTicks`, `_burstEvents`)
  are disabled unless `CTP_NODE_TEST_HOOKS=1` is explicitly set. Never enable
  them in a live process: EventChannel relies on the vendor callback stream being
  its single producer, and lifecycle injection deliberately bypasses a real front.

## Logging / observability

Deliberately, the C++ does **no synchronous logging** — blocking I/O or string
formatting on the CTP callback thread would add exactly the latency/jitter this
design avoids. The hot path stays memcpy + atomics. Observability is layered the
way low-latency systems do it:

- **Counters / events, not logs, in C++.** `droppedRecords` (market data safely
  drops the oldest logical record; Trader overflow is ordered/lossless) and the
  streaming events (`front-connected`, `front-disconnected` with reason, `rsp-*`
  with `errorId`/`errorMsg`) are surfaced to JS.
- **Reasons surfaced, not swallowed.** A pre-trade-risk rejection carries the
  specific cause back to JS (e.g. `blocked by pre-trade risk: order volume
  exceeds maxOrderVolume`; the unsupported-request gate likewise names the mode,
  the unconfirmed session or the configured local controls, and the rate limiter
  has its own message), so logs say *why*. Costs nothing on the hot path — it's
  set only on the (rare) reject.
- **Do the actual logging in JS**, off the callback thread, from those events —
  any async logger (pino/winston) is fine there. If C++-side audit logging is
  ever needed, add an opt-in async/deferred logger (lock-free queue + background
  thread, à la NanoLog/Quill) — never a synchronous one.

## Public TS surface (the shell)

All of this is exposed on `Trader` (`src/trader.ts`):

```ts
// Stateless pre-trade risk (published to the C++ enforcer; takes effect immediately):
td.setRiskLimits({ maxOrderVolume: 5, maxPriceDeviation: 0.02, maxNotional: 2_000_000, maxOrdersPerSec: 20 });
td.trackMarketData(md);                   // C++ reference for the deviation + notional checks
td.reduceOnly(); td.resume();              // block increases; closes/cancels stay available

// Synchronous market-data snapshot from the C++ last-value cache (no event wait):
const tick = md.snapshot("rb2510");       // latest full DepthMarketDataField, or null
const px = md.last("rb2510");             // just the last price (0 if none)

// Multipliers are sourced in C++ from the SPI callback (no JS feed). A plain
// query keeps them current — there is nothing to relay back:
await td.reqQryInstrument({});            // multipliers <- OnRspQryInstrument

// Account facts and account-level caps live in your own trading layer:
const positions = await td.reqQryInvestorPosition({ brokerId, investorId });
const orders = await td.reqQryOrder({ brokerId, investorId });
await td.syncMarginRates(["rb2510"], "1");
const [rate] = td.readMarginRates(["rb2510"], "1"); // resolved schedule for your own margin math
```

## Status

The offline suite covers the stateless risk gate (standalone C++ unit suites),
the generated send path, backpressure and lifecycle isolation. A credential-gated
SimNow integration suite is available separately; it is not evidence of
continuous soak coverage. Live soak remains a separate release gate.

## Process lifecycle: create once, reuse

`MarketData` and `Trader` are **create-once, reuse-for-the-life-of-the-process**
objects. The wrapper constructor calls the native `_start()`, which creates the
vendor API (`CreateFtdcMdApi` / `CreateFtdcTraderApi`) and immediately calls its
`Init()`; `close()` calls the vendor `Release()`
(`CThostFtdcMdApi::Release()` / `CThostFtdcTraderApi::Release()` — see
`doClose()` in `src/native/mdapi.cc` and `src/native/traderapi.cc`).

**Never `Release()` an API that was not `Init()`ed.** The vendor `Release()`
joins an internal reactor thread that only `Init()` starts; on a never-initialised
instance it takes an undefined teardown path that segfaults on Ubuntu 26.04
(glibc 2.43). The native objects therefore create the vendor API only inside
`_start()`, right before `Init()`: a raw object that is closed or garbage-collected
without `_start()` owns no vendor object, and its `close()` releases nothing.

**The observed hazard.** On the bundled Windows vendor DLLs, repeated
`Init()`→`Release()` cycles in one process have reproducibly hung inside
`Release()` — empirically after 4–11 cycles, varying with timing and CTP's
internal reconnect-thread state. The binding cannot recover a process once a
closed-source vendor call is stuck, so this candidate deliberately adopts a
create-once lifecycle. (For comparison: the raw native object *without*
`_start()` survives any number of create/close cycles, since it never creates a
vendor API; the trigger is specifically the `Init()`+`Release()` pairing. A front like `tcp://127.0.0.1:1` that never
connects makes it worse, because CTP's reconnect machinery is what wedges.)

**The rule: never reconnect by destroying and recreating the client.** CTP
reconnects on its own. Handle a dropped link **in place**: when `front-connected`
fires again (it fires on the first connect *and* on every auto-reconnect),
re-run your handshake on the same instance.

```ts
const md = new MarketData("./flow/md/", front);   // construct ONCE
md.on("front-connected", async () => {            // first connect AND every reconnect
  await md.login({ brokerId, userId, password }); // re-login in place
  md.subscribe(["rb2510"]);                        // re-subscribe in place
});

// Trader is the same shape — re-run the one-call handshake on reconnect:
td.on("front-connected", () => td.session({ brokerId, userId, password, appId, authCode }));
```

Call `close()` **once**, at process shutdown (or not at all — process exit tears
everything down cleanly). A reconnect-by-recreate loop will instead wedge the
whole process in the vendor DLL.

**Windows fallout when it does wedge.** A wedged process ignores `SIGTERM` (the
stuck CTP threads never observe the signal), so a timed-out / killed Node
process **lingers** and must be force-killed: `taskkill /F /PID <pid>`. Until it
dies it keeps `build\Release\ctp.node` open, so the next native build can't
overwrite the addon and fails with `LNK1104: cannot open file ... ctp.node`. If
a build suddenly can't write the addon, look for a stray `node` zombie and
`taskkill /F` it first.

**Dev tripwire.** As a backstop against a reconnect-by-recreate bug slipping
into production, `CtpClient` keeps a process-wide count of clients that were
started (`Init`) and then closed (`Release`) and emits a one-time
`console.warn` once that count crosses a threshold (default 3 — deliberately
below the ~4-cycle deadlock point so the warning prints on the `close()`
preceding the typical wedge; a higher value would never fire in time). It runs
only on `close()` — never on the hot data path — so it costs nothing where
latency matters, and a create-once app (one or two long-lived clients closed at
shutdown) never trips it. Tune or disable it with the `CTP_RECREATE_WARN` env
var: `CTP_RECREATE_WARN=0` disables it; any positive integer sets the threshold.

## Backpressure and market-data responses

The market-data ring is `DropPolicy::Oldest`: when the JS consumer falls a full
ring (8192 records) behind a tick flood, the producer uses bounded keep-newest
staging and preferentially displaces an old unread **droppable tick** without
overwriting bytes JS is decoding; the latest staged tick is retained. Request responses (`OnRspUserLogin` /
`OnRspUserLogout` / `OnRspQryMulticastInstrument`) share this ring but are
marked non-droppable: they are retained even when doing so temporarily exceeds
the tick capacity. A stalled JS loop can still delay a response long enough for
the Promise's idle timeout (`requestTimeoutMs`, default 30 s), but the channel
does not deliberately discard it.

The **Trader** ring uses `DropPolicy::Lossless`: after 4096 slots it spills
records into an ordered queue and copies them back as the consumer advances. No
Trader callback is deliberately dropped, at the cost of memory growth during
sustained consumer stalls.
