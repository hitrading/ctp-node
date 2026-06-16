# ctp-node API Reference

Complete usage documentation for every public TypeScript/JavaScript interface in
`ctp-node`. For the architecture and native-hook internals see
[native-hooks.md](native-hooks.md); for the design overview see the
[README](../README.md).

- [Install](#install)
- [Quick start](#quick-start)
- [Core concepts](#core-concepts)
- [`MarketData`](#marketdata) — the market-data (行情) client
- [`Trader`](#trader) — the trading (交易) client
- [`CtpClient`](#ctpclient) — shared base (events, close, backpressure)
- [Types](#types) — `RiskConfig`, `LotCap`, `SessionOptions`, `ArmSpec`, `ArmHandle`, `CallbackOptions`, `RspInfo`, `MdLoginReq`
- [Enums & struct types](#enums--struct-types)
- [Risk controls at a glance](#risk-controls-at-a-glance)

```ts
import {
  MarketData, Trader,
  Direction, OffsetFlag, OrderPriceType, // generated enums
  type RiskConfig, type SessionOptions, type DepthMarketData,
} from "ctp-node";
```

---

## Install

```bash
npm install ctp-node
```

Prebuilt binaries ship for win32-x64, linux-x64 and darwin-x64; other platforms
build from source on install (a C++ toolchain + Python are required, as for any
node-gyp package).

---

## Quick start

### Market data

```ts
import { MarketData } from "ctp-node";

const md = new MarketData("./flow/md/", "tcp://180.168.146.187:10131");

md.on("front-connected", async () => {
  await md.login({ brokerId: "9999", userId: "your-id", password: "your-pw" });
  md.subscribe(["rb2510", "au2508"]);
});

md.on("rtn-depth-market-data", (tick) => {
  console.log(tick.instrumentId, tick.lastPrice, tick.bidPrice1, tick.askPrice1);
});

// keep the process alive; call md.close() once at shutdown
```

### Trading with pre-trade risk

```ts
import { Trader } from "ctp-node";

const td = new Trader("./flow/td/", "tcp://180.168.146.187:10130");

td.riskSet({ maxOrderVolume: 10, maxNotional: 5_000_000, maxOrdersPerSec: 20 });
td.setMaxPosition("rb2510", 100); // never hold more than 100 lots/side

td.on("front-connected", async () => {
  await td.session({
    brokerId: "9999", userId: "your-id", password: "your-pw",
    appId: "your-app", authCode: "your-auth-code",
  });

  // resolves on submission; the fill arrives via rtn-trade
  await td.reqOrderInsert({
    instrumentId: "rb2510",
    direction: Direction.Buy,        // "0"
    combOffsetFlag: OffsetFlag.Open, // "0"
    limitPrice: 3500,
    volumeTotalOriginal: 1,
  }).catch((e) => console.error("order refused:", e.message));
});

td.on("rtn-trade", (t) => console.log("filled", t.instrumentId, t.price, t.volume));
```

---

## Core concepts

**Hybrid event + promise API.** Streaming pushes (ticks, order/trade returns,
connection state) are delivered as **EventEmitter** events with kebab-case names
(`rtn-depth-market-data`, `rtn-order`, …). Request/response calls return a
**Promise** correlated to the response (`login()`, `reqQry*()`, …).

**Create once, reuse for the life of the process.** Construct a `MarketData` /
`Trader` once and keep it. CTP reconnects on its own — handle a dropped link
**in place** by re-running your handshake when `front-connected` fires again (it
fires on the first connect *and* every auto-reconnect). Do **not** destroy and
recreate the client: the vendor DLL deadlocks inside `Release()` after a few
create/close cycles per process. Call `close()` once at shutdown (or not at all —
process exit cleans up). See
[native-hooks.md → Process lifecycle](native-hooks.md#process-lifecycle-create-once-reuse).

**Backpressure.** Each client decodes ticks/returns from a lock-free ring.
`droppedRecords` counts anything dropped when the JS loop falls behind. Market
data drops the **oldest** record (you always see the freshest quote); the trader
drops the **newest**, so an order/trade return is never silently discarded.

**Pre-trade risk runs in C++.** All risk limits (`riskSet`, `setMaxPosition`,
`halt`, …) are enforced on the order-send path inside the native addon, before
the order reaches CTP — no JS round trip. A blocked order makes
`reqOrderInsert()` reject; the reason is in the error message.

---

## `MarketData`

The market-data (行情) client. Subscribes to depth quotes and streams them as
events.

### `new MarketData(flowPath, fronts)`

| Parameter | Type | Meaning |
|---|---|---|
| `flowPath` | `string` | Directory CTP uses for its flow files (caches sequence state). Created if missing; use a per-client path, e.g. `"./flow/md/"`. |
| `fronts` | `string \| string[]` | One or more front addresses, e.g. `"tcp://180.168.146.187:10131"`. Empty/empty-list throws. |

Construction connects asynchronously; wire your handlers, then act in the
`front-connected` handler.

```ts
const md = new MarketData("./flow/md/", "tcp://180.168.146.187:10131");
// or with failover fronts:
const md2 = new MarketData("./flow/md/", [
  "tcp://180.168.146.187:10131",
  "tcp://180.168.146.187:10111",
]);
```

### `md.login(req?)` → `Promise<RspUserLogin>`

Log in to the market-data front. Resolves with the login response, or rejects
with a CTP error (`err.errorId`, `err.errorMsg`).

| Field of `req` (`MdLoginReq`, all optional) | Type | Meaning |
|---|---|---|
| `brokerId` | `string` | Broker id. |
| `userId` | `string` | Investor / user id. |
| `password` | `string` | Password. |
| `tradingDay` | `string` | Trading day (rarely needed). |
| `userProductInfo` | `string` | Product info tag. |

> SimNow's market-data front accepts an anonymous `login({})`; real fronts
> require credentials.

```ts
md.on("front-connected", async () => {
  const rsp = await md.login({ brokerId: "9999", userId: "id", password: "pw" });
  console.log("logged in, trading day", rsp.tradingDay);
  md.subscribe(["rb2510"]);
});
```

### `md.logout(req?)` → `Promise<UserLogout>`

Log out. `req` may carry `{ brokerId?, userId? }`.

### `md.subscribe(instrumentIds)` → `number`

Subscribe to depth market data. Returns the CTP send code (`0` = accepted,
non-zero / `-1` = failed). Quotes arrive as `rtn-depth-market-data` events.

| Parameter | Type | Meaning |
|---|---|---|
| `instrumentIds` | `string[]` | Instrument ids to subscribe, e.g. `["rb2510", "au2508"]`. |

```ts
md.on("rsp-sub-market-data", (info) => console.log("subscribed", info.instrumentId));
md.on("rtn-depth-market-data", (t) => console.log(t.instrumentId, t.lastPrice));
md.subscribe(["rb2510", "au2508"]);
```

### `md.unsubscribe(instrumentIds)` → `number`

Unsubscribe from depth market data. Same shape as `subscribe`.

### `md.subscribeForQuote(instrumentIds)` / `md.unsubscribeForQuote(instrumentIds)` → `number`

Subscribe / unsubscribe to quote-request (询价) notifications, delivered as
`rtn-for-quote` events.

### `md.attachArm(trader)` → `void`

Route this feed's ticks to a `Trader`'s armed triggers (see
[`Trader.arm`](#tdarmmd-spec--armhandle)). Usually you call `td.arm(md, …)`,
which calls this for you. A `MarketData` feeds exactly one `Trader`'s triggers;
attaching a second, different `Trader` throws.

### `md.getApiVersion()` / `md.getTradingDay()` → `string`

The CTP API version string, and the current trading day (`YYYYMMDD`), available
after connect.

### `md.droppedRecords` → `number` (getter)

Total ticks dropped under backpressure (oldest-dropped). Monitor it to detect a
slow consumer. See [`CtpClient`](#ctpclient).

### `md.close()` → `void`

Release the underlying CTP API and free native resources. Idempotent. Call once,
at shutdown. See [lifecycle](#core-concepts).

### MarketData events

Subscribe with `md.on(name, handler)`. Handlers receive `(data, options)` where
`options` is a [`CallbackOptions`](#callbackoptions). Symbolic names are in the
`MarketDataEvent` enum; plain strings work too.

| Event | `data` type | Fires when |
|---|---|---|
| `front-connected` | `undefined` | Connected (first connect and every reconnect). Re-run login/subscribe here. |
| `front-disconnected` | `number` (reason code) | Connection dropped. In-flight requests reject. |
| `rsp-user-login` | `RspUserLogin` | Login response. |
| `rsp-user-logout` | `UserLogout` | Logout response. |
| `rsp-sub-market-data` | `SpecificInstrument` | Subscription ack. |
| `rsp-unsub-market-data` | `SpecificInstrument` | Unsubscription ack. |
| `rtn-depth-market-data` | `DepthMarketData` | A depth quote tick. |
| `rtn-for-quote` | `ForQuoteRsp` | A quote-request notification. |
| `rsp-error` | `undefined` | A CTP error response (`options.rspInfo`). |
| `error` | `unknown` | A *handler* you registered threw — see [error handling](#the-error-event). |

```ts
import { MarketDataEvent } from "ctp-node";
md.on(MarketDataEvent.RtnDepthMarketData, (t) => { /* ... */ });
md.on("front-disconnected", (reason) => console.warn("disconnected", reason));
```

---

## `Trader`

The trading (交易) client: order entry, queries, position/risk tracking, and
latency-critical armed triggers. Extends [`CtpClient`](#ctpclient); every
generated `reqXxx` request method is available (see
[request methods](#generated-request-methods)).

### `new Trader(flowPath, fronts)`

Same parameters as [`MarketData`](#new-marketdataflowpath-fronts). The trader
remembers the credentials from the login response, so `sync*` need no arguments,
and seeds its auto-`OrderRef` counter past the broker's `maxOrderRef` so refs
never collide with a prior session.

```ts
const td = new Trader("./flow/td/", "tcp://180.168.146.187:10130");
```

### `td.session(opts)` → `Promise<{ multipliers, positions, orders }>`

One-call post-connect handshake: **authenticate → login → confirm settlement →
sync multipliers / positions / orders**. Call it from the `front-connected`
handler (and again after a reconnect). Returns the row counts from the sync
steps. Each step just wraps the matching request/sync method, so you can
hand-roll the flow when you need finer control.

`opts` is a [`SessionOptions`](#sessionoptions):

| Field | Type | Meaning |
|---|---|---|
| `brokerId` | `string` | Broker id (required). |
| `userId` | `string` | Investor id (required). |
| `password` | `string` | Password (required). |
| `appId` | `string?` | Terminal app id for authentication. SimNow: `"simnow_client_test"`. |
| `authCode` | `string?` | Terminal auth code. SimNow: 16 zeros. Omit both `appId`/`authCode` to skip authentication. |
| `confirmSettlement` | `boolean?` | Confirm the settlement statement after login (real accounts must, or orders are rejected). Default `true`. |
| `sync` | `object?` | Which risk inputs to fetch after login. `{ multipliers?: boolean \| string[], positions?: boolean, orders?: boolean }`; default fetches all. `multipliers: true` queries every instrument; pass a symbol list to scope it. |

```ts
td.on("front-connected", async () => {
  const counts = await td.session({
    brokerId: "9999", userId: "id", password: "pw",
    appId: "simnow_client_test", authCode: "0000000000000000",
  });
  console.log("synced", counts); // { multipliers, positions, orders }
});

// scope the multiplier sync to the symbols you trade (faster):
await td.session({
  brokerId: "9999", userId: "id", password: "pw",
  sync: { multipliers: ["rb2510", "au2508"], positions: true, orders: true },
});
```

> The hand-rolled equivalent: `await td.reqAuthenticate(...)` →
> `await td.reqUserLogin(...)` → `await td.reqSettlementInfoConfirm(...)` →
> `await td.syncMultipliers()` → `await td.syncPositions()` →
> `await td.syncOrders()`.

### `td.reqOrderInsert(req?)` → `Promise<void>`

Insert an order through the C++ pre-trade risk gate. **Resolves on submission** —
CTP sends no success acknowledgement for an accepted order, only `rtn-order` /
`rtn-trade` (correlate by `orderRef`). It **rejects** only if the send is refused
(risk gate, rate limit, or a CTP API error code). A blank `orderRef` is assigned
a unique numeric value automatically.

`req` is a `Partial<InputOrder>`; the fields you almost always set:

| Field | Type | Meaning |
|---|---|---|
| `instrumentId` | `string` | Contract, e.g. `"rb2510"`. |
| `direction` | `Direction` (`"0"`/`"1"`) | `Buy` / `Sell`. |
| `combOffsetFlag` | `string` | Offset: `"0"` open, `"1"` close, `"3"` close-today, … (one char per leg). |
| `limitPrice` | `number` | Limit price; `0` for a market/any-price order (set `orderPriceType` accordingly). |
| `volumeTotalOriginal` | `number` | Quantity in lots. |
| `orderRef` | `string?` | Leave unset for an auto-assigned unique ref; set it to correlate the resulting `rtn-order`. |
| `orderPriceType` | `OrderPriceType?` | Defaults to limit price; set for market/best-price types. |

```ts
// limit buy 1 lot
await td.reqOrderInsert({
  instrumentId: "rb2510", direction: Direction.Buy,
  combOffsetFlag: OffsetFlag.Open, limitPrice: 3500, volumeTotalOriginal: 1,
});

// handle a refusal (risk gate / rate limit / CTP error)
try {
  await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 9999, volumeTotalOriginal: 1 });
} catch (e) {
  console.warn("refused:", e.message); // e.g. "blocked by pre-trade risk: order price deviates too far from reference"
}

// correlate a fill by your own orderRef
await td.reqOrderInsert({ orderRef: "my-42", instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 2 });
td.on("rtn-trade", (t) => { if (t.orderRef === "my-42") console.log("my order filled", t.volume); });
```

### `td.reqOrderAction(req?)` → `Promise<void>` (cancel)

Cancel / modify a working order. Resolves on submission. Identify the order via
`orderRef` + `frontId` + `sessionId` (from its `rtn-order`), or by
`exchangeId` + `orderSysId`.

```ts
let working;
td.on("rtn-order", (o) => { if (o.orderStatus === "3") working = o; }); // NoTradeQueueing
// ...later:
await td.reqOrderAction({
  instrumentId: working.instrumentId,
  orderRef: working.orderRef, frontId: working.frontId, sessionId: working.sessionId,
  actionFlag: "0", // delete
});
```

### Query methods — `td.reqQry*(req?)` → `Promise<unknown[]>`

Every `reqQry*` resolves with an **array of rows** (multi-row responses are
accumulated; an empty result resolves `[]`). CTP rate-limits queries to roughly
one per second — the [`sync*`](#tdsyncmultipliersinstrumentids--promisenumber)
helpers handle that retry/back-off for you; call raw `reqQry*` sparingly.

```ts
const positions = await td.reqQryInvestorPosition({ brokerId: "9999", investorId: "id" });
const account   = await td.reqQryTradingAccount({ brokerId: "9999", investorId: "id" });
const rb        = await td.reqQryInstrument({ instrumentId: "rb2510" });
```

See [request methods](#generated-request-methods) for the full list.

### `td.syncMultipliers(instrumentIds?)` → `Promise<number>`

Fetch contract multipliers (合约乘数) from CTP and apply them (needed for
multiplier-accurate notional and position-cost limits). No argument queries all
instruments in one request; pass a symbol list to scope it. Retries through
cold-start flow control. Returns the count applied.

```ts
await td.syncMultipliers();                  // all instruments
await td.syncMultipliers(["rb2510", "au2508"]); // just these
```

### `td.syncPositions(opts?)` → `Promise<number>`

Seed open-position cost from CTP (`reqQryInvestorPosition`) into the risk
engine's position tracker. Uses the logged-in account unless
`{ brokerId?, investorId? }` is supplied. Returns the number of positions seeded.

### `td.syncOrders(opts?)` → `Promise<number>`

Rebuild the in-flight reservation from CTP's currently-working orders
(`reqQryOrder`) — an authoritative resync. Call after login and after any
reconnect so the position caps account for orders already working at the broker
(placed before a reconnect, or — since CTP delivers them too — from another
terminal on the same account). Run `syncMultipliers()` first so the reserved cost
uses the right multiplier. Returns the number of working open orders re-reserved.

```ts
await td.syncMultipliers();
await td.syncPositions();
await td.syncOrders(); // now the caps reflect held + working positions
```

### Risk configuration

#### `td.riskSet(config)` → `this`

Publish pre-trade risk limits to the C++ enforcer (takes effect at once). A
non-finite (`NaN`/`Infinity`) limit is **rejected with a throw** rather than
silently disabling the control. `config` is a [`RiskConfig`](#riskconfig); omit a
field or pass `0`/negative to disable that control.

```ts
td.riskSet({
  maxOrderVolume: 10,        // ≤ 10 lots per order
  maxPriceDeviation: 0.02,   // ≤ 2% from the reference price
  maxNotional: 5_000_000,    // ≤ 5M notional per order
  maxOrdersPerSec: 20,       // token-bucket rate limit
  orderBurst: 40,            // bucket size (default: maxOrdersPerSec)
  maxPositionCost: 20_000_000, // ≤ 20M total open-position cost (whole book)
});
```

#### `td.halt()` / `td.resume()` → `this`

Kill-switch. `halt()` immediately blocks every order-opening send that reaches
the exchange at once (regular, exec, quote, for-quote, option-self-close,
comb-action inserts). Cancels and other *actions* stay open so you can still pull
working orders while halted; parked/staged orders are not gated. `resume()`
releases it.

```ts
process.on("SIGINT", () => td.halt()); // stop new orders, keep cancels working
// ...
td.resume();
```

#### `td.setMaxPosition(instrumentId, max)` / `td.setMaxPositions(limits)` → `this`

Cap the open position (in lots) per instrument, enforced on every opening order.
`max` is a [`LotCap`](#lotcap): a number caps both sides; `{ long, short }` caps
each side separately. Within `{ long, short }`, **omitting a side leaves its
current cap unchanged** (pass `0`/negative to clear). Long and short are tracked
independently. The check is fill-based, so a rapid burst of opens can momentarily
overshoot before fills land plus in-flight orders are reserved — size with that
in mind.

```ts
td.setMaxPosition("rb2510", 100);                  // long ≤ 100 and short ≤ 100
td.setMaxPosition("au2508", { long: 50, short: 10 });
td.setMaxPositions({ rb2510: 100, ru2510: { long: 100, short: 20 }, au2508: 10 });
```

#### `td.setMaxPositionCost(instrumentId, maxCost)` / `td.setMaxPositionCosts(limits)` → `this`

Cap one instrument's open-position cost (Σ open price × volume × multiplier, long
and short summed — a gross capital/concentration limit). Per-instrument and
independent of the account-wide `riskSet({ maxPositionCost })`; both apply. Pass
`maxCost ≤ 0` to remove it.

```ts
td.setMaxPositionCost("au2508", 5_000_000);
td.setMaxPositionCosts({ ag2508: 2_000_000, au2508: 5_000_000 });
```

#### `td.setRefPrice(instrumentId, price)` / `td.trackMarketData(md)` → `this`

Provide the reference price the `maxPriceDeviation` check measures against.
`setRefPrice` sets one manually; `trackMarketData(md)` auto-feeds last prices from
a `MarketData` feed. (Without a reference, the deviation check is skipped for that
instrument.)

```ts
td.riskSet({ maxPriceDeviation: 0.02 });
td.trackMarketData(md);          // live reference from the feed
// or, manually:
td.setRefPrice("rb2510", 3500);
```

#### `td.setMultiplier(instrumentId, multiplier)` → `this`

Set a contract's multiplier (合约乘数) for notional / position-cost accounting.
Usually you call [`syncMultipliers()`](#tdsyncmultipliersinstrumentids--promisenumber)
instead of setting these by hand.

```ts
td.setMultiplier("rb2510", 10); // rebar: 10 t/lot
td.setMultiplier("au2508", 1000);
```

### Position-cost tracking

#### `td.seedPosition(instrumentId, side, volume, openCost)` → `this`

Seed a pre-existing position's open cost (for the `maxPositionCost` cap), when you
reconcile positions yourself rather than via `syncPositions()`.

| Parameter | Type | Meaning |
|---|---|---|
| `instrumentId` | `string` | Contract. |
| `side` | `"long" \| "short"` | Position side. |
| `volume` | `number` | Held lots. |
| `openCost` | `number` | Total open cost of that position. |

#### `td.seedFromPositions(positions)` → `this`

Seed from `reqQryInvestorPosition` rows for **gross-mode** accounts
(`posiDirection` `"2"` = long, `"3"` = short). Cost caps sum both sides so the
bucket is harmless for them; for a net-mode instrument under a per-side *volume*
cap, seed via `seedPosition()` with the side you intend.

#### `td.positionCost()` → `number`

Current total open-position cost tracked by the risk engine.

#### `td.resetPositions()` → `this`

Clear all tracked position cost. (`syncPositions()` calls this before re-seeding.)

### Latency-critical armed triggers

#### `td.arm(md, spec)` → `ArmHandle`

Arm a trigger evaluated and fired entirely in C++ on the market-data callback
thread — when `md` sees the condition, the order is sent through this Trader's
risk gate with **no JS round trip**. One-shot. The acknowledgement arrives via
the normal `rtn-order` / `rsp-order-insert` events (correlate by `orderRef`).

`spec` is an [`ArmSpec`](#armspec): `{ instrumentId, side, triggerPrice, order }`.
A **buy** fires when `ask ≤ triggerPrice`; a **sell** when `bid ≥ triggerPrice`.
The `order` is a full `Partial<InputOrder>` and is validated up front (it must
have `instrumentId`, `direction`, `combOffsetFlag` and
`volumeTotalOriginal > 0`), because a malformed template would otherwise misfire
silently from C++.

Returns an [`ArmHandle`](#armhandle) — call `handle.disarm()` to remove it.

```ts
// stop-style: sell rb2510 the instant the bid reaches 3450
const handle = td.arm(md, {
  instrumentId: "rb2510",
  side: "sell",
  triggerPrice: 3450,
  order: {
    instrumentId: "rb2510", direction: Direction.Sell,
    combOffsetFlag: OffsetFlag.CloseToday, limitPrice: 3445, volumeTotalOriginal: 1,
  },
});

// later, cancel the trigger if it hasn't fired
handle.disarm();
```

#### `td.armStats()` → `{ fired: number; blocked: number }`

Observability for armed triggers (they fire in C++ with no JS in the loop):
how many fired and were sent (`fired`) vs were refused by the risk gate / send
(`blocked`). Poll this after a trigger you expected to fire — a blocked armed
order is otherwise invisible.

```ts
const { fired, blocked } = td.armStats();
if (blocked > 0) console.warn("an armed order was refused by risk");
```

### `td.getApiVersion()` / `td.getTradingDay()` → `string`

As on [`MarketData`](#mdgetapiversion--mdgettradingday--string).

### `td.close()` / `td.droppedRecords`

As on [`CtpClient`](#ctpclient). The trader's ring is drop-**newest** (reliable),
so order/trade returns are never silently discarded under backpressure.

### Trader events

| Event | `data` type | Fires when |
|---|---|---|
| `front-connected` | `undefined` | Connected (first connect and every reconnect). Run `session()` here. |
| `front-disconnected` | `number` | Connection dropped. In-flight requests reject. |
| `rsp-user-login` | `RspUserLogin` | Login response. |
| `rtn-order` | `Order` | Order status update (accepted, queueing, filled, cancelled, …). |
| `rtn-trade` | `Trade` | A fill. |
| `err-rtn-order-insert` | `InputOrder` | An order insert was rejected by the exchange (`options.rspInfo`). |
| `error` | `unknown` | A *handler* you registered threw — see below. |

Plus an `rsp-*` event for every request method (e.g. `rsp-qry-investor-position`),
though you'll usually consume those via the request Promise.

```ts
td.on("rtn-order", (o, opts) => {
  console.log(o.instrumentId, "status", o.orderStatus, "ref", o.orderRef);
});
td.on("err-rtn-order-insert", (input, opts) => {
  console.error("exchange rejected", input.orderRef, opts.rspInfo?.errorMsg);
});
```

---

## `CtpClient`

The shared base of `MarketData` and `Trader`. You don't construct it directly,
but its members are available on both clients.

### `client.on(event, handler)` → `this`

Register an event handler (standard `EventEmitter`). Handlers receive
`(data, options)`; `options` is a [`CallbackOptions`](#callbackoptions). All event
names are plain strings (the typed overloads on each subclass list the common
ones); unknown native events surface as `event:<id>`.

### `client.droppedRecords` → `number` (getter)

Total records dropped under backpressure since construction. A steadily climbing
value means the consumer can't keep up. Market data drops oldest; the trader
drops newest.

### `client.close()` → `void`

Release the underlying CTP API and free native resources (the decode ring, SPI,
background threads). Idempotent. In-flight request Promises reject with
`"client closed"`. Call once, at shutdown.

### The `error` event

A throw inside one of *your* event handlers can't be allowed to wedge the data
plane, so it is caught and re-surfaced asynchronously:

- if you subscribe to `error`, the exception is delivered there (catchable);
- otherwise it is re-thrown on the next tick as an `uncaughtException` — the
  normal fate of a throwing `EventEmitter` listener, just deferred so the feed
  stays consistent.

Either way the ring keeps advancing — a buggy handler never re-delivers or stalls
records.

```ts
md.on("error", (err) => console.error("handler threw:", err));
```

### Generated request methods

Every CTP request maps to a camelCase method on `Trader` taking a
`Partial<…Field>` and returning a Promise:

- **`reqQry*`** (queries) → `Promise<unknown[]>` (array of rows; `[]` if empty).
- **Exchange-bound inserts/actions** (`reqOrderInsert`, `reqOrderAction`,
  `reqExecOrderInsert`, `reqQuoteInsert`, `reqForQuoteInsert`,
  `reqOptionSelfCloseInsert`, `reqCombActionInsert`, and their `*Action`
  cancels) → `Promise<void>`, resolving on **submission** (CTP returns no success
  response; outcomes arrive via `rtn-*` events). They reject if the send is
  refused.
- **All other requests** → `Promise<...>` resolving with the single `OnRsp*`
  response row.

Common ones: `reqAuthenticate`, `reqUserLogin`, `reqUserLogout`,
`reqSettlementInfoConfirm`, `reqQrySettlementInfo`, `reqQryInstrument`,
`reqQryInvestorPosition`, `reqQryInvestorPositionDetail`, `reqQryTradingAccount`,
`reqQryOrder`, `reqQryTrade`, `reqOrderInsert`, `reqOrderAction`. (The full set
is generated from the CTP headers — 111 request methods.)

---

## Types

### `RiskConfig`

Pre-trade risk limits for [`riskSet`](#tdriskSetconfig--this). All fields
optional; **omit or `0` = disabled**; `NaN`/`Infinity` throws.

| Field | Type | Meaning |
|---|---|---|
| `maxOrderVolume` | `number` | Max lots per single order. |
| `maxPriceDeviation` | `number` | Max `\|price − reference\| / reference` ratio (e.g. `0.02` = 2%); needs a reference via [`trackMarketData`/`setRefPrice`](#tdsetrefpriceinstrumentid-price--tdtrackmarketdatamd--this). |
| `maxNotional` | `number` | Max notional (price × volume × multiplier) per order. |
| `maxOrdersPerSec` | `number` | Max order sends per second (token bucket). |
| `orderBurst` | `number` | Token-bucket burst size. Default: `maxOrdersPerSec`. |
| `maxPositionCost` | `number` | Cap on total open-position cost across the whole book. |

### `LotCap`

`number | { long?: number; short?: number }` — the cap argument to
[`setMaxPosition`](#tdsetmaxpositioninstrumentid-max--tdsetmaxpositionslimits--this).
A number caps both sides; `{ long, short }` caps each (omit a side = unchanged,
`≤ 0` = clear).

### `SessionOptions`

Options for [`session`](#tdsessionopts--promise-multipliers-positions-orders-).
See the table there. `{ brokerId, userId, password, appId?, authCode?,
confirmSettlement?, sync? }`.

### `ArmSpec`

```ts
interface ArmSpec {
  instrumentId: string;          // contract the trigger watches
  side: "buy" | "sell";          // buy: fires when ask ≤ trigger; sell: when bid ≥ trigger
  triggerPrice: number;          // must be > 0
  order: Partial<InputOrder>;    // the order to send (needs instrumentId, direction,
                                 // combOffsetFlag, volumeTotalOriginal > 0)
}
```

### `ArmHandle`

```ts
interface ArmHandle {
  readonly id: number;
  disarm(): boolean; // remove the trigger; false if it was already gone/fired
}
```

### `CallbackOptions`

The second argument to every event handler:

```ts
interface CallbackOptions {
  requestId?: number;   // the request this response correlates to (responses only)
  isLast?: boolean;     // last row of a multi-row response (responses only)
  rspInfo?: RspInfo;    // present when the record carries a CTP error
}
```

### `RspInfo`

```ts
interface RspInfo {
  errorId: number;      // 0 = success
  errorMsg: string;     // human-readable (GB18030-decoded)
}
```

A rejected request Promise's error carries these fields too:
`err.errorId` / `err.errorMsg`.

### `MdLoginReq`

```ts
interface MdLoginReq {
  brokerId?: string; userId?: string; password?: string;
  tradingDay?: string; userProductInfo?: string;
}
```

---

## Enums & struct types

All CTP enums (values) and struct interfaces (types) are generated from the CTP
headers and re-exported from the package root.

**Enums** are string-valued and usable both as the enum member and the raw code:

```ts
import { Direction, OffsetFlag, OrderStatus, OrderPriceType } from "ctp-node";

Direction.Buy;          // "0"
Direction.Sell;         // "1"
OffsetFlag.Open;        // "0"
OffsetFlag.CloseToday;  // "3"
OrderStatus.AllTraded;  // "0"

// both forms type-check on a struct field:
const a = { direction: Direction.Buy };
const b = { direction: "0" } as const;
```

**Struct types** describe the shape of event payloads and request fields — e.g.
`DepthMarketData` (a tick), `Order`, `Trade`, `InputOrder`,
`InvestorPosition`, `RspUserLogin`. Import them as types:

```ts
import type { DepthMarketData, Order, Trade, InputOrder } from "ctp-node";

md.on("rtn-depth-market-data", (tick: DepthMarketData) => {
  const mid = (tick.bidPrice1 + tick.askPrice1) / 2;
});
```

> Field names are camelCase versions of the CTP fields (`LastPrice` →
> `lastPrice`, `BidPrice1` → `bidPrice1`). CTP fills unset numeric price fields
> with a sentinel (`Number.MAX_VALUE`); guard for it if you read a price that may
> not have traded yet (the risk engine already does).

---

## Risk controls at a glance

| Control | Set via | Scope | Disabled when |
|---|---|---|---|
| Kill-switch | `halt()` / `resume()` | All opening sends | `resume()` |
| Max order volume | `riskSet({ maxOrderVolume })` | Per order | `0` / omitted |
| Max price deviation | `riskSet({ maxPriceDeviation })` + reference | Per order | `0` / no reference |
| Max notional | `riskSet({ maxNotional })` | Per order | `0` / omitted |
| Rate limit | `riskSet({ maxOrdersPerSec, orderBurst })` | Per second | `0` / omitted |
| Max position lots | `setMaxPosition(id, …)` | Per instrument, per side | `≤ 0` |
| Max position cost (per instrument) | `setMaxPositionCost(id, …)` | Per instrument | `≤ 0` |
| Max position cost (book) | `riskSet({ maxPositionCost })` | Whole book | `0` / omitted |

Position-lot and position-cost caps count **committed = held (filled) +
in-flight (working order) volume**, so a burst of opens can't slip past a cap
before the fills report. Feed held positions via `syncPositions()` /
`seedPosition()` and working orders via `syncOrders()` (especially after a
reconnect). All caps are enforced in C++ on the send path; a breach makes
`reqOrderInsert()` reject with the reason in the message.
