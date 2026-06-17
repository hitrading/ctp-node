<!-- LANG-SWITCH -->
**English** · [简体中文](native-hooks.zh-CN.md)

# Native-sinkable hooks (risk / rate-limit / arm)

Goal: keep ~95% of strategy work in JS/TS (fast iteration, good ecosystem) while
giving the **latency- and safety-critical** slices C++-grade determinism — no GC
pause or event-loop hiccup can defeat them. JS sets the rules; C++ enforces and
executes them.

## Two worlds

| | C++ world | JS world |
|---|---|---|
| Runs on | CTP callback thread | Node main thread (event loop) |
| GC pauses | none | yes (occasional ms-tens-of-ms) |
| Determinism | microsecond, predictable | jitter (GC / JIT / scheduling) |

A tick first arrives on the **C++ callback thread**. A pure-JS strategy must
cross to JS and wait for the event loop — eating that jitter. "Sinking down"
keeps the critical decision on the C++ thread; JS only configures and is
notified afterward.

## What is sunk down

1. **Risk (pre-trade)** — `src/native/risk.h` `RiskEngine`
   Hard checks run before every send: kill-switch, max order volume, max
   single-order notional (fat-finger guard), price-deviation guard, open-MARGIN
   caps (account-wide and per-instrument), and a per-instrument max position
   (lots, capped per side). Enforced in C++ so they hold even if the JS process
   is mid-GC, blocked, or buggy. *Safety win - worth it even for slow
   strategies.*
   - Status: **all real**. The price-deviation guard reads its reference from
     the C++ MD snapshot cache (the LVC; see §4 below) via `trackMarketData(md)`
     — no `setRefPrice`, no JS round-trip, and it covers armed orders too. The
     single-order notional check is still notional (price × volume × multiplier
     — a fat-finger guard). The open-position cap is REAL MARGIN: Σ(price ×
     volume × multiplier × marginRate) per instrument (long/short, with
     proportional release on close), updated from `OnRtnTrade` on the trader
     callback thread.
   - **Architecture principle: the C++ risk engine sources all CTP-derived data
     directly from the SPI callbacks — it never relies on JS to relay data that
     C++ already receives.** Special-cased in `scripts/codegen/generate-trader.mjs`
     (emitted into `traderspi.gen.cc`):
     - contract multipliers ← `OnRspQryInstrument` (`setMultiplier`)
     - per-contract margin rates ← `OnRspQryInstrumentMarginRate`
       (`setMarginRate`; larger of long/short by-money = a conservative single
       rate). Until a rate arrives, a conservative default (full notional)
       applies, so the cap can only over-count — it never under-blocks.
     - login session (FrontID/SessionID, for reservation keys) ← `OnRspUserLogin`
       (`setSession`), published synchronously so reservation keys are correct
       before any order can be sent
     - fills ← `OnRtnTrade` (`onTrade`)
     - order updates ← `OnRtnOrder` (`onOrderUpdate`)

     So any `reqQryInstrument` keeps multipliers current and querying
     `reqQryInstrumentMarginRate` for the contracts you trade keeps the margin
     cap exact — there is no `setMultiplier` / `setMarginRate` to call from JS
     (those C++ entry points exist only for tests / offline use). Multipliers
     and margin rates are static metadata stored separately from position state,
     so `resetPositions()` / a `syncPositions()` resync never wipes them.
   - In-flight reservation: the position caps count `held + working` (orders
     sent but not yet filled), so a burst of opens can't slip past before fills
     report. Reservations are tracked per `(FrontID, SessionID, OrderRef)` and
     reconciled from `OnRtnOrder` (self-correcting; released on fill/cancel/
     reject and on a failed send). Because CTP delivers an investor's order/
     trade returns to *all* their sessions, this also accounts for orders placed
     from **another terminal on the same account** (their fills update the
     position by instrument; their working orders consume the cap).
   - What stays JS-driven (NOT auto-sourced): `seedPosition` / `syncPositions`
     (open-position seeding) and `rebuildReservations` / `syncOrders` (rebuild
     the reservation from the broker's working orders, via `reqQryOrder`). These
     are deliberate resync operations with RESET semantics — they clear and
     re-seed their book — so they stay explicit, JS-orchestrated calls; auto-
     running them on every query would be a dangerous side-effect. Call
     `syncOrders()` after login and after any reconnect.

2. **Rate limit** — `src/native/risk.h` `RateLimiter`
   Token bucket guaranteeing the seat never exceeds the exchange/CTP order rate,
   immune to JS bursts after a pause.
   - Status: **real** (token bucket).

3. **Arm (latency-critical trigger)** — `src/native/arm.h` `ArmRegistry`
   A narrow rule ("fire the instant ask ≤ X") evaluated on the MD callback
   thread, parameterized from JS. Only for genuinely latency-sensitive
   strategies.
   - Status: **wired**. `trader.arm(md, spec)` shares the registry (shared_ptr)
     with the MarketData feeding ticks; on a hit the order is built from the
     JS-encoded template, passed through the Trader's `RiskEngine`, and sent via
     `ReqOrderInsert` on the MD callback thread (no JS). One-shot; ack via normal
     trader events. Teardown-safe: the sink is cleared under the registry lock
     before the API is released.

4. **Market-data snapshot (last-value cache)** — `src/native/snapshot.h`
   `SnapshotCache`
   A C++ last-value cache (LVC) holding the latest full `DepthMarketDataField`
   per instrument. It's updated on **every** tick — in `mdspi.cc`'s
   `OnRtnDepthMarketData`, before the record is published to the ring — so the
   freshest quote is always available without waiting for the event loop to
   drain. Cleared per-instrument on unsubscribe (`erase`) and entirely on close
   (`clear`).
   - **JS reads it synchronously**: `md.snapshot(id)` returns the full latest
     tick (or `null` if none seen / unsubscribed), `md.last(id)` returns just
     the last price — no waiting for a tick, no per-tick bookkeeping in your
     strategy.
   - **The deviation reference flows MD→risk entirely in C++.** The Trader's
     price-deviation check reads the cache in C++ via `trackMarketData(md)`: the
     MarketData shares the cache as a `shared_ptr` (mirroring how the armed-order
     registry is shared), so the risk engine reads live prices straight from the
     MD feed with no JS round-trip — and the check therefore also covers armed
     orders that fire from C++ with no JS in the loop. (The `SnapshotCache`
     implements `RefPriceSource`; the risk engine holds it via `setMdSnapshot`.)

## Threading / safety model

- Config is written on the **slow path** (JS thread) and read on the **hot
  path** (JS or callback thread). Risk config uses **atomics** (lock-free
  reads); the rate limiter uses a tiny mutex (bounded, uncontended).
- `ArmRegistry` currently guards its list with a mutex; TODO is a read-mostly
  lock-free snapshot so `onTick` never blocks on rare arm/disarm writes.

## Logging / observability

Deliberately, the C++ does **no synchronous logging** — blocking I/O or string
formatting on the CTP callback thread would add exactly the latency/jitter this
design avoids. The hot path stays memcpy + atomics. Observability is layered the
way low-latency systems do it:

- **Counters / events, not logs, in C++.** `droppedRecords` (backpressure —
  market data drops the oldest record to keep the freshest quote, the trader
  drops the newest so a queued order/trade return is never lost),
  arm fire count, and the streaming events (`front-connected`,
  `front-disconnected` with reason, `rsp-*` with `errorId`/`errorMsg`) are
  surfaced to JS.
- **Reasons surfaced, not swallowed.** A pre-trade-risk rejection carries the
  specific cause back to JS (e.g. `blocked by pre-trade risk: order volume
  exceeds maxOrderVolume`; position caps and rate limit have their own
  messages), so logs say *why*. Costs nothing on the hot path — it's set only
  on the (rare) reject.
- **Do the actual logging in JS**, off the callback thread, from those events —
  any async logger (pino/winston) is fine there. If C++-side audit logging is
  ever needed, add an opt-in async/deferred logger (lock-free queue + background
  thread, à la NanoLog/Quill) — never a synchronous one.

## Public TS surface (the shell)

All of this is exposed on `Trader` (`src/trader.ts`):

```ts
// Pre-trade risk (published to the C++ enforcer; takes effect immediately):
td.riskSet({ maxOrderVolume: 5, maxPriceDeviation: 0.02, maxOrdersPerSec: 20, maxMargin: 5_000_000 });
td.trackMarketData(md);                   // wire the C++ MD snapshot in as the deviation reference
td.setMaxPositions({ rb2610: 100, ru2610: { long: 100, short: 20 } }); // per-instrument lot caps
td.setMaxPositionCosts({ ag2608: 2_000_000, au2608: 5_000_000 });      // per-instrument open-margin caps
td.halt(); td.resume();                   // kill-switch (C++ blocks all sends instantly)

// Synchronous market-data snapshot from the C++ last-value cache (no event wait):
const tick = md.snapshot("rb2510");       // latest full DepthMarketDataField, or null
const px = md.last("rb2510");             // just the last price (0 if none)

// Multipliers and margin rates are sourced in C++ from the SPI callbacks (no JS
// feed). A plain query keeps them current — there is nothing to relay back:
await td.reqQryInstrument({});            // multipliers <- OnRspQryInstrument
await td.reqQryInstrumentMarginRate({ instrumentId: "rb2510" }); // rate <- OnRspQryInstrumentMarginRate

// Deliberate JS-driven resyncs (RESET semantics — call explicitly, not per query):
await td.syncPositions();                 // reset + seed open-position margin (reqQryInvestorPosition)
await td.syncOrders();                    // rebuild in-flight reservations (reqQryOrder) — after every reconnect

// Latency-critical armed trigger (fires from C++ on the MD thread):
const armed = td.arm(md, {
  instrumentId: "rb2510", side: "buy", triggerPrice: 3500,
  order: { /* InputOrder; orderRef auto-assigned if blank */ },
});
armed.disarm();                           // remove the trigger
// One-shot; the fill/ack arrives via the normal rtn-order / rtn-trade events.
```

## Status

Fully wired and live-verified against SimNow: risk gate (volume / single-order
notional / deviation / open-margin / kill-switch) and rate limiter on the order
send path, `ArmRegistry::onTick` fed from the MD callback firing `ReqOrderInsert`
through the risk gate with no JS hop, per-instrument position state, the C++ MD
snapshot cache feeding the deviation reference, and a real-margin open-position
cap (multiplier × margin-rate, both sourced in C++ from the SPI callbacks).

## Process lifecycle: create once, reuse

`MarketData` and `Trader` are **create-once, reuse-for-the-life-of-the-process**
objects. The wrapper constructor calls the native `_start()`, which calls the
vendor `Init()`; `close()` calls the vendor `Release()`
(`CThostFtdcMdApi::Release()` / `CThostFtdcTraderApi::Release()` — see
`doClose()` in `src/native/mdapi.cc` and `src/native/traderapi.cc`).

**The hazard.** The CTP vendor DLLs (`thostmduserapi_se.dll` /
`thosttraderapi_se.dll`) **deadlock inside `Release()` after a handful of
`Init()`→`Release()` cycles in a single process** — empirically 4–11 cycles,
varying with timing and CTP's internal reconnect-thread state. This is a
well-known limitation of the CTP SDK itself: its API objects are *not* safe to
create / `Init` / `Release` repeatedly within one process. It is **not** a bug
in this binding's C++/TS, and it **cannot be fixed here** — the hang is inside
the closed-source vendor DLL. (For confirmation: the raw native object *without*
`_start()` survives 80+ create/close cycles; the trigger is specifically the
`Init()`+`Release()` pairing. A front like `tcp://127.0.0.1:1` that never
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
ring (8192 records) behind a tick flood, the producer overwrites the oldest
unread slots. Request **responses** (`OnRspUserLogin` / `OnRspUserLogout` /
`OnRspQryMulticastInstrument`) share this ring with the streaming ticks, so a
response can in principle be dropped if ≥ 8192 ticks pile up after it before the
event loop drains — the awaiting Promise would then reject on the idle timeout
(`requestTimeoutMs`, default 30 s) rather than resolve.

In practice this is benign: `login()` runs at startup before any subscription, so
no ticks are flowing; the only responses that *could* be caught behind a live
full-market flood are `logout()` (issued at shutdown) and the rarely-used
`reqQryMulticastInstrument()`. The failure is bounded (a timeout rejection, never
a silent hang or corruption) and recoverable (retry). If you must query market
data mid-session under heavy load, expect a possible timeout and retry. The
**trader** ring is `DropPolicy::Newest` (reliable): order/trade returns and all
trader responses are never dropped under backpressure, so order-flow correctness
is unaffected regardless of market-data load.
