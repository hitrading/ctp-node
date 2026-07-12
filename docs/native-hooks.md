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
   - Status: **implemented with a deliberately bounded product model**. The price-deviation guard reads its reference from
     the C++ MD snapshot cache (the LVC; see §4 below) via `trackMarketData(md)`
     — no `setRefPrice`, no JS round-trip, and it covers armed orders too. The
     single-order notional check is still notional (price × volume × multiplier
     — a fat-finger guard). Futures/TAS open-order margin uses the CTP formula
     `price × volume × multiplier × byMoney + volume × byVolume`, separately by
     side/hedge flag. Held snapshots use CTP `UseMargin` and retain separate
     instrument/side/hedge buckets so a close releases the matching schedule;
     relative investor
     schedules are added to the matching exchange base schedule. This is a
     conservative local gate, not a universal replica of every broker margin
     algorithm; unsupported product classes fail closed under monetary caps.
   - **Architecture principle: latency-sensitive metadata and realtime ledger
     events flow directly from SPI callbacks into C++.** Authoritative position
     and working-order query snapshots are aggregated in TypeScript, then
     committed through revision-checked native batch operations. Special-cased
     in `scripts/codegen/generate-trader.mjs`
     (emitted into `traderspi.gen.cc`):
     - contract multipliers ← `OnRspQryInstrument` (internal `setInstrument`)
     - per-contract/hedge margin schedules ← `OnRspQryInstrumentMarginRate`.
       Long/short by-money and by-volume components are retained separately.
       `OnRspQryExchangeMarginRate` supplies the base for `IsRelative` investor
       schedules. Missing multipliers or either required schedule fail closed.
     - login session (FrontID/SessionID, for reservation keys) ← `OnRspUserLogin`
       (`setSession`), published synchronously so reservation keys are correct
       before any order can be sent
     - fills ← `OnRtnTrade` (`onTrade`)
     - order updates ← `OnRtnOrder` (`onOrderUpdate`)

     So any `reqQryInstrument` keeps multipliers current and querying
     `reqQryInstrumentMarginRate` for the contracts you trade keeps the margin
     cap schedule current — there is no JS metadata relay
     (those C++ entry points exist only for tests / offline use). Multipliers
     and margin rates are static metadata stored separately from position state,
     so `resetPositions()` / a `syncPositions()` resync never wipes them.
   - In-flight reservation: the position caps count `held + working` (orders
     sent but not yet filled), so a burst of opens can't slip past before fills
     report. Reservations are tracked per `(FrontID, SessionID, OrderRef)` and
     reconciled from `OnRtnOrder` (self-correcting; released on fill/cancel/
     reject and on a failed send). Terminal tombstones also retain the concrete
     trading-day/exchange/instrument/`OrderSysID` fingerprint: matching late
     replays are ignored, while a reused or ambiguous key fails closed until an
     authoritative `syncOrders()` snapshot supersedes it. Because CTP delivers an investor's order/
     trade returns to *all* their sessions, this also accounts for orders placed
     from **another terminal on the same account** (their fills update the
     position by instrument; their working orders consume the cap).
     Trade idempotency keeps at most 65,536 exact fingerprints, retiring complete
     older days first. Exhausting that budget inside one active trading day does
     not evict ambiguous keys: opening inserts stay fail-closed until a later
     trading day is observed and `syncPositions()` reconciles the missed epoch.
   - What stays JS-driven (NOT auto-sourced): `seedPositions` / `syncPositions`
     (open-position seeding) and `rebuildReservations` / `syncOrders` (rebuild
     the reservation from the broker's working orders, via `reqQryOrder`). These
     are deliberate resync operations. They apply revision-checked atomic
     replacements, preserve working reservations, and retry if realtime callbacks
     race the query. They stay explicit, JS-orchestrated calls; auto-
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
     `ReqOrderInsert` on the MD callback thread (no JS). A successful send removes
     the arm immediately; halt/rate/metadata/send refusal keeps it armed with a
     retry backoff. Ack arrives via normal trader events. Teardown-safe: the sink is cleared under the registry lock
     before the API is released.

4. **Market-data snapshot (last-value cache)** — `src/native/snapshot.h`
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
   - **The deviation reference flows MD→risk entirely in C++.** The Trader's
     price-deviation check reads the cache in C++ via `trackMarketData(md)`: the
     MarketData shares the cache as a `shared_ptr` (mirroring how the armed-order
     registry is shared), so the risk engine reads live prices straight from the
     MD feed with no JS round-trip — and the check therefore also covers armed
     orders that fire from C++ with no JS in the loop. Unauthenticated or
     post-disconnect ticks remain observable protocol events but cannot drive
     the cache or an armed mandate. (The `SnapshotCache`
     implements `RefPriceSource`; the risk engine holds it via `setMdSnapshot`.)

## Threading / safety model

- Config is written on the **slow path** (JS thread) and read on the **hot
  path** (JS or callback thread). Risk config uses **atomics** (lock-free
  reads); the rate limiter uses a tiny mutex (bounded, uncontended).
- `ArmRegistry` is indexed by instrument, so `onTick` scans only that contract's
  active triggers (successful arms are erased immediately). A short mutex keeps
  arm/disarm and Trader teardown linearizable with an in-flight send.
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
  drops the oldest logical record; Trader overflow is ordered/lossless),
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
td.setRiskLimits({ maxOrderVolume: 5, maxPriceDeviation: 0.02, maxOrdersPerSec: 20, maxAccountMargin: 5_000_000 });
td.trackMarketData(md);                   // C++ reference for deviation + monetary valuation
td.setInstrumentPositionLimits({ rb2610: 100, ru2610: { long: 100, short: 20 } });
td.setInstrumentMarginLimits({ ag2608: 2_000_000, au2608: 5_000_000 }); // per-instrument margin caps
td.reduceOnly(); td.resume();              // block increases; closes/cancels stay available

// Synchronous market-data snapshot from the C++ last-value cache (no event wait):
const tick = md.snapshot("rb2510");       // latest full DepthMarketDataField, or null
const px = md.last("rb2510");             // just the last price (0 if none)

// Multipliers and margin rates are sourced in C++ from the SPI callbacks (no JS
// feed). A plain query keeps them current — there is nothing to relay back:
await td.reqQryInstrument({});            // multipliers <- OnRspQryInstrument
await td.reqQryInstrumentMarginRate({ instrumentId: "rb2510" }); // rate <- OnRspQryInstrumentMarginRate

// Deliberate JS-driven atomic resyncs (call explicitly, not per query):
await td.syncOrders();                    // rebuild reservations; marks held side uncertain
await td.syncPositions();                 // final held snapshot; preserve reservations

// Latency-critical armed trigger (fires from C++ on the MD thread):
const armed = td.arm(md, {
  instrumentId: "rb2510", side: "buy", triggerPrice: 3500,
  order: { /* InputOrder; orderRef auto-assigned if blank */ },
});
armed.disarm();                           // remove the trigger
// One-shot; the fill/ack arrives via the normal rtn-order / rtn-trade events.
```

## Status

The offline suite covers the risk ledger, arm state machine, generated send path,
backpressure and lifecycle isolation. A credential-gated SimNow integration suite
is available separately; it is not evidence of continuous soak coverage. Margin
estimation models both formula components and relative+exchange-base schedules;
missing metadata fails closed. Live soak remains a separate release gate.

## Process lifecycle: create once, reuse

`MarketData` and `Trader` are **create-once, reuse-for-the-life-of-the-process**
objects. The wrapper constructor calls the native `_start()`, which calls the
vendor `Init()`; `close()` calls the vendor `Release()`
(`CThostFtdcMdApi::Release()` / `CThostFtdcTraderApi::Release()` — see
`doClose()` in `src/native/mdapi.cc` and `src/native/traderapi.cc`).

**The observed hazard.** On the bundled Windows vendor DLLs, repeated
`Init()`→`Release()` cycles in one process have reproducibly hung inside
`Release()` — empirically after 4–11 cycles, varying with timing and CTP's
internal reconnect-thread state. The binding cannot recover a process once a
closed-source vendor call is stuck, so this candidate deliberately adopts a
create-once lifecycle. (For comparison: the raw native object *without*
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
