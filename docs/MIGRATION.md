# Migrating 5.x → 6.0

6.0 tightens the session handshake, response matching and rate attribution.
Three changes are breaking for code that relied on the old behaviour:

| Change | Before (5.x) | Now (6.0) | What to do |
|---|---|---|---|
| `reqSettlementInfoConfirm()` result | the first response row (object) | **every** response row (`unknown[]`); a legal empty final packet adds none | iterate the rows; reading `.brokerId` on the result now yields `undefined` |
| `CtpClient` subclasses: `request(send, responseEvent)` / `queryRequest(send, responseEvent)` | second argument was `single: boolean` | second argument is the **expected response event**; only that callback (or `rsp-error`) settles the request, a same-id callback of another type is ignored and named in the idle-timeout error | pass the event code of the matching `OnRspXxx` (the generated `reqXxx` methods already do) |
| Raw `reqUserLogin()` | the login response published `broker`/`investor` (the operator UserID as investor) | publishes no order-capable identity; `reqOrderInsert()` and `syncMarginRates()` after a hand-rolled login reject with `a confirmed Trader login is required` / `syncMarginRates requires a successful trader login first` | use `session()`, which confirms the trading account natively and in JS only after the settlement confirmation succeeds (or is explicitly skipped) on the same connection |

Other 6.0 behaviour:

- `session()` checks every settlement-confirmation row: a row naming another
  broker or investor fails the handshake. A failure after the account
  confirmation (metadata or margin sync) revokes the session identity.
- `syncMarginRates()` checks each rate row's ownership and investor range (see
  *Behaviour changes* below and the API reference).
- Instrument metadata from `reqQryInstrument()` is staged per request and
  published only on the query's successful final packet; an error packet, a
  disconnect or a newer catalog query discards it. While a full catalog query
  is in flight, an enabled `maxNotional` check refuses openings for lack of a
  multiplier, so run catalog/margin synchronization before you start trading.

# Migrating 4.x → 5.0

5.0 makes the native pre-trade risk gate **stateless** and removes armed orders
(`arm()`: local orders fired by a market-data trigger). These are not CTP's
parked orders: `reqParkedOrderInsert` and the other parked-order requests remain
and are handled as unsupported requests (forwarded only in `NORMAL`, for a
confirmed session, with no local control configured; `-10006` otherwise).
Every native rule now judges one order from the order itself, the latest
market-data reference and static instrument metadata. C++ no longer keeps a
position or working-order ledger, so the account-level exposure caps — account
margin, per-instrument margin and position lots — move to the trading layer that
owns the account facts.

Why: CTP's position snapshot and its realtime order/trade stream share no common
sequence number. A ledger inside the binding could only mark itself uncertain
after every close, refuse openings until a resync, and guess at empty snapshots
from a funds cross-check — duplicating the caller's own books and able to
disagree with them. Armed orders fired straight from the market-data thread,
bypassing every business check the caller runs before sending.

TypeScript reports every removed member at compile time. In JavaScript, a call
to a removed method throws `TypeError: ... is not a function`, and the removed
option fields are rejected rather than ignored:
`setRiskLimits({ maxAccountMargin })` throws synchronously
(`TypeError: setRiskLimits: unknown field maxAccountMargin`), while
`session({ sync: { positions, orders } })` — `session()` is async — returns a
Promise that rejects with `TypeError: session.sync: unknown field positions`.

**The removed getters fail silently in JavaScript.** `Trader.accountMargin` and
`Trader.armStats` were getters; reading them now yields `undefined` and throws
nothing. A cap check such as `td.accountMargin + est > cap` becomes
`NaN > cap`, which is always `false`, so the check silently lets every order
through. Search your whole code base for `accountMargin` and `armStats` and
replace every read before upgrading (see the table below).

## Removed APIs

| 4.x | 5.0 replacement |
|---|---|
| `RiskLimits.maxAccountMargin` | Your trading layer compares its own margin figure (for example `reqQryTradingAccount` → `currMargin` + `frozenMargin`, plus an estimate for the order about to be sent) with your cap before calling `reqOrderInsert()`. |
| `Trader.setInstrumentPositionLimit()` / `setInstrumentPositionLimits()`, type `InstrumentPositionLimit` | Per-instrument lot caps in your trading layer, from positions (`reqQryInvestorPosition`) and working orders (`reqQryOrder`) kept current with `rtn-order` / `rtn-trade`. |
| `Trader.setInstrumentMarginLimit()` / `setInstrumentMarginLimits()` | Per-instrument margin caps in your trading layer; multipliers from `reqQryInstrument`, schedules from `syncMarginRates()` / `readMarginRates()`. |
| `Trader.syncPositions()`, `seedPositions()`, `seedFromPositions()`, `resetPositions()`, type `PositionSeed` | `reqQryInvestorPosition({ brokerId, investorId })` into your own position book. |
| `Trader.syncOrders()` | `reqQryOrder({ brokerId, investorId })` into your own order book. |
| `Trader.accountMargin` (getter) | `reqQryTradingAccount({ brokerId, investorId })` (`currMargin`, `frozenMargin`, `available`), or your own book. **JavaScript reads now return `undefined` without throwing**: `td.accountMargin + est > cap` is `NaN > cap`, always `false`, so the cap silently passes — search for and replace every read. |
| `session()` options `sync.positions` / `sync.orders`; result fields `positions` / `orders` | Query positions and orders yourself after `session()` resolves. The result is now `{ identity, multipliers, marginRates }`. |
| `Trader.arm()`, `MarketData.attachArm()`, types `ArmSpec` / `ArmHandle` | Evaluate the trigger in your own process (in the `rtn-depth-market-data` handler, or from `md.snapshot()`), run your normal pre-send checks, then call `reqOrderInsert()`. CTP's parked-order requests (`reqParkedOrderInsert` etc.) are unrelated and remain. |
| `Trader.armStats` (getter) | Count triggers and sends in your own trigger code. **JavaScript reads now return `undefined` without throwing**, so arithmetic on it yields `NaN` and comparisons are silently `false` — search for and replace every read. |
| `err.marginRejection` on a `-10003` rejection | Removed with `-10003`. A native rejection carries `err.ctpReturnCode` and the reason in `err.message`. |

## Retired return codes

| Code | 4.x meaning | 5.0 |
|---|---|---|
| `-10003` | blocked by a margin cap | Retired with the margin caps. |
| `-10004` | blocked by a position-lot cap | Retired with the lot caps. |
| `-10005` | duplicate `OrderRef` in this front/session | Retired. The Trader still rejects an explicit `orderRef` that is not above its high-water mark (`RangeError`), and the CTP counter itself rejects a duplicate (`不允许重复报单`). |
| `-10007` | risk metadata or ledger unavailable | Retired. A missing or stale reference price or a missing multiplier for an enabled rule is reported as `-10001`, with the reason in the message. |

Unchanged: `-10001` (stateless gate refusal), `-10002` (rate limit), `-10006`
(unsupported request or mode), `-10008` (invalid order fields), `-10009`
(session transition in flight).

## Behaviour changes

- **Opening no longer waits for a resync.** 4.x kept opening orders fail-closed
  after every handshake until both `syncOrders()` and `syncPositions()` had
  committed, and again after any close fill. In 5.0 an opening order is admitted
  as soon as `session()` has confirmed the account and the configured stateless
  rules pass.
- **Unsupported requests are no longer lifetime-sticky.** In 4.x, once CTP had
  accepted an advanced request (quote/exec/comb/parked insert, order modify,
  bank/futures transfer) in pure binding mode — or one was observed from another
  terminal — later enabling a local control kept ordinary opens fail-closed. In
  5.0 each request is judged on its own: advanced requests still pass only in
  `NORMAL`, for a confirmed session, with no local control configured (`-10006`
  otherwise), and they leave no state behind.
- **Orders from other terminals no longer affect admission.** 4.x counted their
  working orders against the lot caps and blocked opens while an unsupported
  (for example multi-leg) working order existed. CTP delivers the returns of the
  investor's other sessions too, so your own book can account for them.
- **`syncMarginRates()` feeds only `readMarginRates()`.** Schedules are resolved
  in TypeScript for your own calculations; they are no longer published to the
  native gate.
- **From 6.0 `syncMarginRates()` checks each rate row's attribution and
  investor range.** An investor row must name the confirmed BrokerID, carry an
  empty InvestUnitID (the Trader sends no investment unit) and match by
  InvestorRange: single investor (`3`) — the confirmed InvestorID; investor
  group (`2`) — the account's InvestorGroupID from `reqQryInvestor`, queried
  only when such a row appears, and unavailable means the sync fails; all
  investors (`1`) — no InvestorID check, since it may carry a wildcard. Single
  investor overrides group, which overrides all; a contract row still overrides
  a product row. An exchange base row must name the confirmed BrokerID.
  Disagreeing rows of one range (or of one contract's exchange base) fail the
  sync instead of the last row winning, and the "rate is missing" error names
  the rows excluded as belonging to other accounts.
- The 3.0 notes below about the position ledger turning uncertain after close
  fills, and about calling `syncPositions()` after `syncOrders()`, no longer
  apply.

Kept as before: `maxOrderVolume`, `maxPriceDeviation`, `maxReferenceAgeMs`,
`maxNotional`, the open/close rate limiters, `reduceOnly()` / `haltAll()` /
`resume()`, `trackMarketData()`, `syncMultipliers()`, `syncMarginRates()` /
`readMarginRates()`, `frontInfo` and the per-front query pacing, the `session()`
handshake with its account confirmation, and the single-instrument GFD/FAK/FOK
limit-order allowlist.

## Before / after

Account caps:

```ts
// 4.x
td.setRiskLimits({ maxOrderVolume: 10, maxAccountMargin: 2_000_000 });
td.setInstrumentPositionLimit("rb2610", 100);
await td.session({ ...creds }); // also synced orders + positions into the C++ ledger

// 5.0
td.setRiskLimits({ maxOrderVolume: 10 }); // per-order gates stay in C++
// Subscribe first and buffer: returns keep arriving while the queries run.
let ready = false, generation = 0;
const pending = [];
td.on("rtn-order", (o) => (ready ? book.onOrder(o) : pending.push(["order", o])));
td.on("rtn-trade", (t) => (ready ? book.onTrade(t) : pending.push(["trade", t])));
td.on("front-connected", () => {
  generation++; ready = false; pending.length = 0; // the new snapshot supersedes them
  /* ...and run this recovery again */
});

const started = generation;
// The confirmed trading account. Pass investorId explicitly whenever the login
// UserID is not the InvestorID; never query with the login user ID.
const { identity } = await td.session({ ...creds, investorId: creds.investorId });
const acct = { brokerId: creds.brokerId, investorId: creds.investorId };
const orders = await td.reqQryOrder(acct);
const trades = await td.reqQryTrade(acct);
const positions = await td.reqQryInvestorPosition(acct);
const funds = (await td.reqQryTradingAccount({ ...acct, currencyId: "CNY" })).filter((f) =>
  f.brokerId === acct.brokerId && f.accountId === acct.investorId && f.currencyId === "CNY");
if (generation !== started || funds.length !== 1 || funds[0].tradingDay !== td.tradingDay)
  throw new Error("snapshot does not belong to this session; run the recovery again");
book.reset({ identity, orders, trades, positions, funds: funds[0] });
// Merge the buffered returns (the book dedupes by order and trade identity)...
for (const [kind, row] of pending.splice(0))
  kind === "order" ? book.onOrder(row) : book.onTrade(row);
ready = true; // ...and only then admit new exposure.
// Check your caps against `book` before every reqOrderInsert().
```

`book` stands for your own ledger; the binding does not provide one. Query with
the investor ID that `session()` confirmed, not the login user ID, and never
take the first funds record unchecked: verify its broker, account, currency and
trading day, and that no reconnect (a new session generation) happened while
the snapshot was taken. A return that arrives while the queries run may or may
not be in the snapshot; merge the buffered returns, deduplicated, before
opening new exposure.

Armed order:

```ts
// 4.x
td.arm(md, { instrumentId: "rb2610", side: "sell", triggerPrice: 3450, order: exitOrder });

// 5.0
md.on("rtn-depth-market-data", (t) => {
  if (t.instrumentId !== "rb2610") return;
  const bid = t.bidPrice1;
  if (!(bid > 0 && bid < 1e300) || bid < 3450) return; // skip the DBL_MAX no-bid sentinel
  // Hand the intent to your one trading gateway; the trigger never sends.
  gateway.submit({ intentKey: "rb2610-exit", order: exitOrder });
});
```

A resolved `reqOrderInsert()` means only that the request was sent. The
counter rejects asynchronously — `rsp-order-insert` / `err-rtn-order-insert`,
or an `rtn-order` that ends cancelled — and that rejection never reaches a
`catch` around the call. (The call itself throws synchronously on invalid
parameters and rejects when the local send is refused: risk gate, rate limit,
API return code. Those orders were definitely not sent.) So the gateway, the
only component that calls `reqOrderInsert()`, owns the order: it deduplicates
intents by key, tracks each sent order by its identity (FrontID, SessionID,
OrderRef) until a terminal `rtn-order` or a rejection, and reports a definite
rejection to the strategy, which decides whether to retry. When delivery is in
doubt — a disconnect or timeout after the send — the gateway first reconciles
with `reqQryOrder` and the returns, and never blindly sends again.

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
  install (Windows and Linux prebuilds are unchanged). Use a release that
  bundles the 6.7.13 Mac frameworks (4.1.3-rc.2 or later): from 1.2.0 the
  bundled CTP macOS frameworks were still 6.7.7 under the 6.7.13 headers, so a
  macOS source build linked but crashed at runtime (4.1.3-rc.1 refused to build
  instead).
- **Node `engines`** is restricted to the range required by `node-gyp` 13
  (`^22.22.2 || ^24.15.0 || >=26.0.0`); Node 20 and older 22 lines are excluded.
