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
   Hard checks run before every send: kill-switch, max order volume, price
   deviation (fat-finger), max notional, max total open-position cost, and a
   per-instrument max position (lots, capped per side). Enforced in C++ so they
   hold even if the JS process is mid-GC, blocked, or buggy. *Safety win —
   worth it even for slow strategies.*
   - Status: **all real**. Price deviation uses a per-instrument reference fed
     from market data (`setRefPrice` / `trackMarketData`). Notional and
     position-cost both apply the contract multiplier (price × volume ×
     multiplier). Position-cost tracks Σ(open cost) per instrument (long/short,
     with proportional release on close), updated from `OnRtnTrade` on the
     trader callback thread.
   - Multipliers and pre-existing positions are fetched from CTP automatically:
     `syncMultipliers()` (via `reqQryInstrument`) and `syncPositions()` (via
     `reqQryInvestorPosition`) — no manual `setMultiplier` / `seedPosition`
     needed (those remain for tests / offline use). Multipliers are stored
     separately from position state, so `syncPositions()` (which resets and
     re-seeds positions) never wipes them.

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

## Threading / safety model

- Config is written on the **slow path** (JS thread) and read on the **hot
  path** (JS or callback thread). Risk config uses **atomics** (lock-free
  reads); the rate limiter uses a tiny mutex (bounded, uncontended).
- `ArmRegistry` currently guards its list with a mutex; TODO is a read-mostly
  lock-free snapshot so `onTick` never blocks on rare arm/disarm writes.

## Public TS surface (the shell)

All of this is exposed on `Trader` (`src/trader.ts`):

```ts
// Pre-trade risk (published to the C++ enforcer; takes effect immediately):
td.riskSet({ maxOrderVolume: 5, maxPriceDeviation: 0.02, maxOrdersPerSec: 20, maxPositionCost: 5_000_000 });
td.trackMarketData(md);                   // feed the deviation/notional reference
td.setMaxPositions({ rb2610: 100, ru2610: { long: 100, short: 20 } }); // per-instrument lot caps
td.halt(); td.resume();                   // kill-switch (C++ blocks all sends instantly)

// Risk inputs auto-fetched from CTP after login:
await td.syncMultipliers();               // contract multipliers (reqQryInstrument)
await td.syncPositions();                 // existing open-position cost (reqQryInvestorPosition)

// Latency-critical armed trigger (fires from C++ on the MD thread):
const armed = td.arm(md, {
  instrumentId: "rb2510", side: "buy", triggerPrice: 3500,
  order: { /* InputOrder; orderRef auto-assigned if blank */ },
});
armed.disarm();                           // remove the trigger
// One-shot; the fill/ack arrives via the normal rtn-order / rtn-trade events.
```

## Status

Fully wired and live-verified against SimNow: risk gate (volume / deviation /
notional / position-cost / kill-switch) and rate limiter on the order send
path, `ArmRegistry::onTick` fed from the MD callback firing `ReqOrderInsert`
through the risk gate with no JS hop, per-instrument position state, and
multiplier-accurate notional + position cost.
