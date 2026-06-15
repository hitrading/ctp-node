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
   deviation (fat-finger), max notional. Enforced in C++ so they hold even if
   the JS process is mid-GC, blocked, or buggy. *Safety win — worth it even for
   slow strategies.*
   - Status: **real** (kill-switch, volume, deviation, notional).
   - TODO: per-instrument net-position limits (needs live position state from
     the trade-return path).

2. **Rate limit** — `src/native/risk.h` `RateLimiter`
   Token bucket guaranteeing the seat never exceeds the exchange/CTP order rate,
   immune to JS bursts after a pause.
   - Status: **real** (token bucket).

3. **Arm (latency-critical trigger)** — `src/native/arm.h` `ArmRegistry`
   A narrow rule ("fire the instant ask ≤ X") evaluated on the callback thread,
   parameterized from JS. Only for genuinely latency-sensitive strategies.
   - Status: registry + tick matching **real**; the actual order fire is
     **reserved (TODO)** until the Trader send path exists.

## Threading / safety model

- Config is written on the **slow path** (JS thread) and read on the **hot
  path** (JS or callback thread). Risk config uses **atomics** (lock-free
  reads); the rate limiter uses a tiny mutex (bounded, uncontended).
- `ArmRegistry` currently guards its list with a mutex; TODO is a read-mostly
  lock-free snapshot so `onTick` never blocks on rare arm/disarm writes.

## Public TS surface (the shell)

See `src/api/hooks.ts`: `RiskApi` (`set` / `halt` / `resume`), `ArmSpec` /
`ArmHandle`, and `NativeHooks` (`risk`, `arm(...)`). These lock the contract; a
`Trader` will implement `NativeHooks` once its send path is built.

```ts
trader.risk.set({ maxOrderVolume: 5, maxPriceDeviation: 0.02, maxOrdersPerSec: 20 });
trader.risk.halt();                       // C++ blocks all sends instantly
const armed = trader.arm({ instrumentId: "rb2510", side: "buy", triggerPrice: 3500, volume: 1 });
armed.on("fired", (o) => { /* post-hoc bookkeeping, latency irrelevant */ });
```

## Wiring checklist (later milestones)

- [ ] Trader native class + `reqOrderInsert` send path
- [ ] Route `RiskEngine::check` + `allowRate` into that send path
- [ ] Feed depth ticks into `ArmRegistry::onTick` from the MD callback
- [ ] `ArmRegistry` fire callback → send path; async-notify JS
- [ ] Per-instrument position state → position-limit risk rule
- [ ] Contract multiplier → accurate notional
