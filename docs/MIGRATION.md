# Migrating 2.x → 3.0

3.0 reshapes the risk API and moves risk data sourcing into C++. Most renames
are caught by the TypeScript compiler, but a few changes keep the **same field
name with different semantics** — re-check these explicitly.

## ⚠️ Silent semantic changes (same name, different behaviour)

- **`RiskLimits.maxNotional` now multiplies by the contract multiplier.**
  The cap is `price × volume × multiplier` (was `price × volume`). A value
  carried over from 2.x therefore permits roughly `multiplier`× the notional you
  previously configured (e.g. ~10× for a ×10 contract). **Re-derive your
  `maxNotional`.** It also only applies to futures/TAS; other product classes
  (e.g. options) fail closed for `maxNotional`.
- **`seedFromPositions()` / `syncPositions()` reject net-mode rows.**
  `posiDirection` must be `'2'` (long) or `'3'` (short); net (`'1'`) is no longer
  bucketed as short — it throws. Long/short position mode is required.
- **Explicit `orderRef` must be strictly greater than the session high-water
  mark.** A reused or non-monotonic explicit `orderRef` is rejected client-side
  (`RangeError`) before it reaches CTP.

## Renamed / restructured APIs

| 2.x | 3.0 |
|---|---|
| `RiskConfig` | `RiskLimits` |
| `RiskLimits.maxMargin` / `maxPositionCost` | `maxAccountMargin` |
| `LotCap` | `InstrumentPositionLimit` |
| `Trader.halt()` | `Trader.reduceOnly()` (block opens, allow closes) **and** `Trader.haltAll()` (block both) |
| `Trader.seedPosition(one)` | `Trader.seedPositions([...])` (atomic replace) + `PositionSeed` |
| `Trader.setMultiplier` / `setMarginRate` (JS) | sourced in C++ from CTP; fed via `session()` / `syncMultipliers()` / `syncMarginRates()` |
| `getApiVersion()` / `getTradingDay()` | `apiVersion` / `tradingDay` getters |

Margin rates are **no longer fed from JS** — they come from CTP's margin-rate
queries inside C++. Enabled monetary caps **fail closed** until the multiplier
and margin schedule for an instrument have arrived (call `session()` or the
`sync*` helpers after login).

## Fail-closed behaviour to be aware of

- After **any close fill**, the position ledger is marked uncertain and opening
  orders fail closed (`CTP_RISK_METADATA_UNAVAILABLE`) until an authoritative
  `syncPositions()` — this prevents a delayed realtime close (for a fill already
  in a prior snapshot) from double-releasing exposure. Re-run `syncPositions()`
  to resume opening after closes.
- `syncOrders()` marks the held-position side uncertain; run `syncPositions()`
  **after** it.

## Platform / toolchain

- **macOS** is no longer shipped as a prebuild — it builds from source on
  install (Windows and Linux prebuilds are unchanged).
- **Node `engines`** is restricted to the range required by `node-gyp` 13
  (`^22.22.2 || ^24.15.0 || >=26.0.0`); Node 20 and older 22 lines are excluded.
