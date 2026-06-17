<!-- LANG-SWITCH -->
**English** · [简体中文](API.zh-CN.md)

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
- [Recipes](#recipes) — strategy skeleton · reconnect handling · combination orders

```ts
import {
  MarketData, Trader,
  Direction, OffsetFlag, OrderPriceType, // generated enums
  type RiskConfig, type SessionOptions, type DepthMarketData,
} from "@hitrading/ctp-node";
```

---

## Install

```bash
npm install @hitrading/ctp-node
```

Prebuilt binaries ship for win32-x64, linux-x64 and darwin-x64; other platforms
build from source on install (a C++ toolchain + Python are required, as for any
node-gyp package).

---

## Quick start

### Market data

```ts
import { MarketData } from "@hitrading/ctp-node";

const md = new MarketData("./flow/md/", "tcp://182.254.243.31:30012");

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
import { Trader, Direction, OffsetFlag } from "@hitrading/ctp-node";

const td = new Trader("./flow/td/", "tcp://182.254.243.31:30002");

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
| `fronts` | `string \| string[]` | One or more front addresses, e.g. `"tcp://182.254.243.31:30012"`. Empty/empty-list throws. |

Construction connects asynchronously; wire your handlers, then act in the
`front-connected` handler.

```ts
// single front
const md = new MarketData("./flow/md/", "tcp://182.254.243.31:30012");

// multiple fronts for failover
const md2 = new MarketData("./flow/md/", [
  "tcp://182.254.243.31:30012",
  "tcp://182.254.243.31:30011",
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
// with credentials
md.on("front-connected", async () => {
  const rsp = await md.login({ brokerId: "9999", userId: "id", password: "pw" });
  console.log("logged in, trading day", rsp.tradingDay);
  md.subscribe(["rb2510"]);
});

// anonymous (SimNow MD front)
await md.login();

// handle a bad login
try {
  await md.login({ brokerId: "9999", userId: "id", password: "wrong" });
} catch (e) {
  console.error("login failed", e.errorId, e.errorMsg);
}
```

### `md.logout(req?)` → `Promise<UserLogout>`

Log out. `req` may carry `{ brokerId?, userId? }`.

```ts
await md.logout();
// or scoped to a specific account
await md.logout({ brokerId: "9999", userId: "id" });
```

### `md.subscribe(instrumentIds)` → `number`

Subscribe to depth market data. Returns the CTP send code (`0` = accepted,
non-zero / `-1` = failed). Quotes arrive as `rtn-depth-market-data` events.

| Parameter | Type | Meaning |
|---|---|---|
| `instrumentIds` | `string[]` | Instrument ids to subscribe, e.g. `["rb2510", "au2508"]`. |

```ts
md.on("rsp-sub-market-data", (info) => console.log("subscribed", info.instrumentId));
md.on("rtn-depth-market-data", (t) => console.log(t.instrumentId, t.lastPrice));

const rc = md.subscribe(["rb2510", "au2508"]);
if (rc !== 0) console.warn("subscribe send failed:", rc);

// subscribe more later (e.g. after rolling to a new contract)
md.subscribe(["rb2601"]);
```

### `md.unsubscribe(instrumentIds)` → `number`

Unsubscribe from depth market data. Same shape as `subscribe`.

```ts
md.unsubscribe(["au2508"]);          // stop one
md.unsubscribe(["rb2510", "au2508"]); // stop several
```

### `md.subscribeForQuote(instrumentIds)` / `md.unsubscribeForQuote(instrumentIds)` → `number`

Subscribe / unsubscribe to quote-request (询价) notifications, delivered as
`rtn-for-quote` events (used by options market makers).

```ts
md.on("rtn-for-quote", (q) => console.log("quote requested for", q.instrumentId));
md.subscribeForQuote(["IO2508-C-3900"]);
// ...later
md.unsubscribeForQuote(["IO2508-C-3900"]);
```

### `md.snapshot(instrumentId)` → `DepthMarketData | null`

The latest full depth tick for an instrument, read **synchronously** from the
C++ last-value cache. The cache is updated on **every** tick before it reaches
JS, so there is no waiting for an event and no per-tick bookkeeping in your
strategy. Returns `null` if no tick has been seen for that instrument (or its
entry was cleared). The entry is cleared on `unsubscribe`, and all entries on
`close`.

| Parameter | Type | Meaning |
|---|---|---|
| `instrumentId` | `string` | Instrument id, e.g. `"rb2510"`. |

```ts
md.subscribe(["rb2510"]);
// ...later, anywhere in your code — no event handler needed
const tick = md.snapshot("rb2510");
if (tick) console.log("mid", (tick.bidPrice1 + tick.askPrice1) / 2, "@", tick.updateTime);
```

### `md.last(instrumentId)` → `number`

The latest price for an instrument from the same C++ cache, read
**synchronously**. Returns `0` if no tick has been seen (or the entry was
cleared — see [`snapshot`](#mdsnapshotinstrumentid--depthmarketdata--null)).

```ts
const px = md.last("rb2510");
if (px > 0) console.log("last traded", px);
```

### `md.attachArm(trader)` → `void`

Route this feed's ticks to a `Trader`'s armed triggers (see
[`Trader.arm`](#tdarmmd-spec--armhandle)). Usually you call `td.arm(md, …)`,
which calls this for you. A `MarketData` feeds exactly one `Trader`'s triggers;
attaching a second, different `Trader` throws.

```ts
// explicit (rarely needed — td.arm() does this automatically):
md.attachArm(td);

// the second, different trader throws:
md.attachArm(td);        // ok
md.attachArm(otherTd);   // throws: already feeds another Trader
```

### `md.getApiVersion()` → `string` / `md.getTradingDay()` → `string`

The CTP API version string, and the current trading day (`YYYYMMDD`), available
after connect.

```ts
console.log("CTP MD API", md.getApiVersion()); // e.g. "6.7.2"
md.on("front-connected", () => console.log("trading day", md.getTradingDay()));
```

### `md.droppedRecords` → `number` (getter)

Total ticks dropped under backpressure (oldest-dropped). Monitor it to detect a
slow consumer.

```ts
setInterval(() => {
  if (md.droppedRecords > 0) console.warn("MD dropped", md.droppedRecords, "ticks");
}, 5000);
```

### `md.close()` → `void`

Release the underlying CTP API and free native resources. Idempotent. Call once,
at shutdown. See [lifecycle](#core-concepts).

```ts
process.on("SIGINT", () => { md.close(); process.exit(0); });
```

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
import { MarketDataEvent } from "@hitrading/ctp-node";

// symbolic name
md.on(MarketDataEvent.RtnDepthMarketData, (t) => {
  const mid = (t.bidPrice1 + t.askPrice1) / 2;
});

// reconnect handling
md.on("front-connected", async () => { await md.login(); md.subscribe(["rb2510"]); });
md.on("front-disconnected", (reason) => console.warn("MD disconnected, code", reason));
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
const td = new Trader("./flow/td/", "tcp://182.254.243.31:30002");
// failover fronts:
const td2 = new Trader("./flow/td/", ["tcp://182.254.243.31:30002", "tcp://182.254.243.31:30001"]);
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
// SimNow full handshake
td.on("front-connected", async () => {
  const counts = await td.session({
    brokerId: "9999", userId: "id", password: "pw",
    appId: "simnow_client_test", authCode: "0000000000000000",
  });
  console.log("synced", counts); // { multipliers, positions, orders }
});

// scope the multiplier sync to the symbols you trade (faster cold start)
await td.session({
  brokerId: "9999", userId: "id", password: "pw",
  sync: { multipliers: ["rb2510", "au2508"], positions: true, orders: true },
});

// skip the settlement confirm (environments that don't need it)
await td.session({ brokerId: "9999", userId: "id", password: "pw", confirmSettlement: false });
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
import { Direction, OffsetFlag } from "@hitrading/ctp-node";

// 1) limit buy 1 lot to open
await td.reqOrderInsert({
  instrumentId: "rb2510", direction: Direction.Buy,
  combOffsetFlag: OffsetFlag.Open, limitPrice: 3500, volumeTotalOriginal: 1,
});

// 2) handle a refusal (risk gate / rate limit / CTP error)
try {
  await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 9999, volumeTotalOriginal: 1 });
} catch (e) {
  console.warn("refused:", e.message); // e.g. "blocked by pre-trade risk: order price deviates too far from reference"
}

// 3) correlate a fill by your own orderRef
await td.reqOrderInsert({ orderRef: "my-42", instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 2 });
td.on("rtn-trade", (t) => { if (t.orderRef === "my-42") console.log("my order filled", t.volume); });

// 4) close-today a short position (sell -> close)
await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseToday, limitPrice: 3490, volumeTotalOriginal: 1 });
```

### `td.reqOrderAction(req?)` → `Promise<void>` (cancel)

Cancel / modify a working order. Resolves on submission. Identify the order via
`orderRef` + `frontId` + `sessionId` (from its `rtn-order`), or by
`exchangeId` + `orderSysId`.

```ts
import { ActionFlag } from "@hitrading/ctp-node";

let working;
td.on("rtn-order", (o) => { if (o.orderStatus === "3") working = o; }); // NoTradeQueueing

// cancel it
await td.reqOrderAction({
  instrumentId: working.instrumentId,
  orderRef: working.orderRef, frontId: working.frontId, sessionId: working.sessionId,
  actionFlag: ActionFlag.Delete, // "0"
});

// cancel by exchange order id instead
await td.reqOrderAction({
  instrumentId: working.instrumentId,
  exchangeId: working.exchangeId, orderSysId: working.orderSysId,
  actionFlag: "0",
});
```

### Query methods — `td.reqQry*(req?)` → `Promise<unknown[]>`

Every `reqQry*` resolves with an **array of rows** (multi-row responses are
accumulated; an empty result resolves `[]`). CTP rate-limits queries to roughly
one per second — the [`sync*`](#tdsyncmultipliersinstrumentids--promisenumber)
helpers handle that retry/back-off for you; call raw `reqQry*` sparingly.

```ts
// current positions
const positions = await td.reqQryInvestorPosition({ brokerId: "9999", investorId: "id" });
for (const p of positions) console.log(p.instrumentId, p.posiDirection, p.position);

// account funds
const [account] = await td.reqQryTradingAccount({ brokerId: "9999", investorId: "id" });
console.log("available", account?.available);

// instrument details (multiplier, tick size, …)
const [rb] = await td.reqQryInstrument({ instrumentId: "rb2510" });
console.log("multiplier", rb?.volumeMultiple, "tick", rb?.priceTick);

// today's orders / trades
const orders = await td.reqQryOrder({ brokerId: "9999", investorId: "id" });
const trades = await td.reqQryTrade({ brokerId: "9999", investorId: "id" });
```

See [request methods](#generated-request-methods) for the full list.

### `td.syncMultipliers(instrumentIds?)` → `Promise<number>`

Trigger the instrument query so the C++ risk engine picks up contract multipliers
(合约乘数) — it sources them **directly** from CTP's `OnRspQryInstrument` response,
so this no longer calls a JS setter. Needed for multiplier-accurate notional and
position-margin limits. No argument queries all instruments in one request; pass a
symbol list to scope it. Retries through cold-start flow control. Returns the count
of contracts that came back with a multiplier.

```ts
const n = await td.syncMultipliers();          // all instruments
await td.syncMultipliers(["rb2510", "au2508"]); // just these
console.log(n, "contracts with a multiplier");
```

### `td.syncPositions(opts?)` → `Promise<number>`

Seed open-position **margin** from CTP (`reqQryInvestorPosition`, each row's
`UseMargin`) into the risk engine's position tracker. Uses the logged-in account
unless `{ brokerId?, investorId? }` is supplied. Returns the number of positions
seeded.

```ts
const held = await td.syncPositions();
console.log("seeded", held, "positions, margin =", td.positionCost());
// explicit account
await td.syncPositions({ brokerId: "9999", investorId: "id" });
```

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
const working = await td.syncOrders();
console.log("re-reserved", working, "working open orders");
```

### Risk configuration

#### `td.riskSet(config)` → `this`

Publish pre-trade risk limits to the C++ enforcer (takes effect at once). A
non-finite (`NaN`/`Infinity`) limit is **rejected with a throw** rather than
silently disabling the control. `config` is a [`RiskConfig`](#riskconfig); omit a
field or pass `0`/negative to disable that control.

```ts
// full set
td.riskSet({
  maxOrderVolume: 10,          // ≤ 10 lots per order
  maxPriceDeviation: 0.02,     // ≤ 2% from the reference price
  maxNotional: 5_000_000,      // ≤ 5M notional per order
  maxOrdersPerSec: 20,         // token-bucket rate limit
  orderBurst: 40,              // bucket size (default: maxOrdersPerSec)
  maxMargin: 20_000_000,       // ≤ 20M total open-position MARGIN (whole book)
});

// just a couple of controls (the rest stay disabled)
td.riskSet({ maxOrderVolume: 5, maxOrdersPerSec: 10 });

// disable a control by passing 0
td.riskSet({ maxNotional: 0 });
```

#### `td.halt()` / `td.resume()` → `this`

Kill-switch. `halt()` immediately blocks every order-opening send that reaches
the exchange at once (regular, exec, quote, for-quote, option-self-close,
comb-action inserts). Cancels and other *actions* stay open so you can still pull
working orders while halted; parked/staged orders are not gated. `resume()`
releases it.

```ts
// stop all new orders on a panic signal, but keep cancels working
process.on("SIGINT", () => td.halt());

// re-enable trading once you've assessed the situation
td.resume();

// example: halt if PnL breaches a limit
td.on("rtn-trade", () => { if (computePnl() < -100000) td.halt(); });
```

#### `td.setMaxPosition(instrumentId, max)` / `td.setMaxPositions(limits)` → `this`

Cap the open position (in lots) per instrument, enforced on every opening order.
`max` is a [`LotCap`](#lotcap): a number caps both sides; `{ long, short }` caps
each side separately. Within `{ long, short }`, **omitting a side leaves its
current cap unchanged** (pass `0`/negative to clear). Long and short are tracked
independently. The check counts committed = held + in-flight (working) volume.

```ts
td.setMaxPosition("rb2510", 100);                  // long ≤ 100 and short ≤ 100
td.setMaxPosition("au2508", { long: 50, short: 10 }); // asymmetric

// raise only the long cap, leave short as it was
td.setMaxPosition("au2508", { long: 80 });
// clear the short cap
td.setMaxPosition("au2508", { short: 0 });

// many at once
td.setMaxPositions({ rb2510: 100, ru2510: { long: 100, short: 20 }, au2508: 10 });
```

#### `td.setMaxPositionCost(instrumentId, maxCost)` / `td.setMaxPositionCosts(limits)` → `this`

Cap one instrument's open-position **margin** (Σ price × volume × multiplier ×
marginRate, long and short summed — a per-contract capital/concentration limit).
Per-instrument and independent of the account-wide
`riskSet({ maxMargin })`; both apply. Pass `maxCost ≤ 0` to remove it. Until a
contract's margin rate is known (fed automatically from CTP — query
`reqQryInstrumentMarginRate`), it counts at full notional (conservative).

```ts
td.setMaxPositionCost("au2508", 5_000_000); // ≤ 5M of gold margin
td.setMaxPositionCosts({ ag2508: 2_000_000, au2508: 5_000_000 });
td.setMaxPositionCost("au2508", 0);         // remove the cap
```

#### `td.trackMarketData(md)` → `this`

Provide the live reference price the `maxPriceDeviation` check measures against.
Wires the `MarketData` feed's C++ snapshot cache straight into this Trader's risk
engine, so the deviation reference is read **in C++ on the order-send path** — no
JS round-trip, and it covers armed orders that fire from C++ with no JS in the
loop. **Without it, no reference is set and the deviation check is skipped.**

```ts
td.riskSet({ maxPriceDeviation: 0.02 });

// live reference from the feed (read in C++ on every order, incl. armed orders)
td.trackMarketData(md);
```

> Contract multipliers (for notional / cost accounting) and per-contract margin
> rates (for the `maxMargin` cap) are **not** fed from JS: the C++ risk engine
> sources them directly from CTP's `OnRspQryInstrument` /
> `OnRspQryInstrumentMarginRate` callbacks. So any `reqQryInstrument` keeps
> multipliers current (see [`syncMultipliers`](#tdsyncmultipliersinstrumentids--promisenumber)),
> and querying `reqQryInstrumentMarginRate` for the contracts you trade keeps the
> margin cap exact — no JS setter, and the checks also cover armed orders.

### Position-margin tracking

#### `td.seedPosition(instrumentId, side, volume, margin)` → `this`

Seed a pre-existing position's real **margin** (for the `maxMargin` /
`maxPositionCost` cap), when you reconcile positions yourself rather than via
`syncPositions()`. `margin` is the actual capital occupied — CTP
`InvestorPosition.UseMargin` — the unit the margin cap tracks.

| Parameter | Type | Meaning |
|---|---|---|
| `instrumentId` | `string` | Contract. |
| `side` | `"long" \| "short"` | Position side. |
| `volume` | `number` | Held lots. |
| `margin` | `number` | Real margin occupied by that position (`InvestorPosition.UseMargin`). |

```ts
// you already hold 3 long lots of rb2510 using 30,000 margin
td.seedPosition("rb2510", "long", 3, 30_000);
// and 2 short lots of au2508 using 112,000 margin
td.seedPosition("au2508", "short", 2, 112_000);
```

#### `td.seedFromPositions(positions)` → `this`

Seed real open-position **margin** from `reqQryInvestorPosition` rows (each row's
`UseMargin`) for **gross-mode** accounts (`posiDirection` `"2"` = long, `"3"` =
short). Margin caps sum both sides so the bucket is harmless for them; for a
net-mode instrument under a per-side *volume* cap, seed via `seedPosition()` with
the side you intend.

```ts
const rows = await td.reqQryInvestorPosition({ brokerId: "9999", investorId: "id" });
td.resetPositions();
td.seedFromPositions(rows); // (this is exactly what syncPositions() does)
```

#### `td.positionCost()` → `number`

Current total open-position margin tracked by the risk engine (the unit the
`maxMargin` / `maxPositionCost` cap measures).

```ts
console.log("book margin", td.positionCost());
td.on("rtn-trade", () => console.log("margin now", td.positionCost()));
```

#### `td.resetPositions()` → `this`

Clear all tracked position margin. (`syncPositions()` calls this before re-seeding.)

```ts
td.resetPositions();              // wipe the tracker
td.seedFromPositions(freshRows);  // re-seed from a fresh query
```

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
`volumeTotalOriginal > 0`).

Returns an [`ArmHandle`](#armhandle) — call `handle.disarm()` to remove it.

```ts
import { Direction, OffsetFlag } from "@hitrading/ctp-node";

// stop-loss: sell rb2510 the instant the bid reaches 3450
const stop = td.arm(md, {
  instrumentId: "rb2510",
  side: "sell",
  triggerPrice: 3450,
  order: {
    instrumentId: "rb2510", direction: Direction.Sell,
    combOffsetFlag: OffsetFlag.CloseToday, limitPrice: 3445, volumeTotalOriginal: 1,
  },
});

// breakout entry: buy when the ask reaches 3600
const entry = td.arm(md, {
  instrumentId: "rb2510", side: "buy", triggerPrice: 3600,
  order: { instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3605, volumeTotalOriginal: 2 },
});

// cancel a trigger that hasn't fired
stop.disarm();
```

#### `td.armStats()` → `{ fired: number; blocked: number }`

Observability for armed triggers (they fire in C++ with no JS in the loop):
how many fired and were sent (`fired`) vs were refused by the risk gate / send
(`blocked`). Poll this after a trigger you expected to fire — a blocked armed
order is otherwise invisible.

```ts
const { fired, blocked } = td.armStats();
console.log(`armed: ${fired} sent, ${blocked} refused`);
if (blocked > 0) console.warn("an armed order was refused by risk");
```

### `td.getApiVersion()` / `td.getTradingDay()` → `string`

The CTP trader API version and current trading day.

```ts
console.log("CTP TD API", td.getApiVersion(), "day", td.getTradingDay());
```

### `td.close()` / `td.droppedRecords`

As on [`CtpClient`](#ctpclient). The trader's ring is drop-**newest** (reliable),
so order/trade returns are never silently discarded under backpressure.

```ts
console.log("trader dropped (should stay 0):", td.droppedRecords);
process.on("SIGINT", () => td.close());
```

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
td.on("rtn-trade", (t) => console.log("FILL", t.instrumentId, t.price, "x", t.volume));
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

```ts
md.on("rtn-depth-market-data", (tick, options) => {
  if (options.rspInfo) console.error("error record", options.rspInfo.errorMsg);
  else console.log(tick.lastPrice);
});

// one-shot with .once, remove with .off — it's a normal EventEmitter
td.once("front-connected", () => console.log("connected for the first time"));
```

### `client.droppedRecords` → `number` (getter)

Total records dropped under backpressure since construction. A steadily climbing
value means the consumer can't keep up. Market data drops oldest; the trader
drops newest.

```ts
setInterval(() => console.log("dropped:", md.droppedRecords, td.droppedRecords), 10_000);
```

### `client.close()` → `void`

Release the underlying CTP API and free native resources (the decode ring, SPI,
background threads). Idempotent. In-flight request Promises reject with
`"client closed"`. Call once, at shutdown.

```ts
async function shutdown() { td.close(); md.close(); }
process.on("SIGTERM", shutdown);
```

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
md.on("error", (err) => console.error("a market-data handler threw:", err));
td.on("error", (err) => console.error("a trader handler threw:", err));
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

```ts
// authenticate / login / confirm manually (what session() automates)
await td.reqAuthenticate({ brokerId: "9999", userId: "id", appId: "simnow_client_test", authCode: "0000000000000000" });
await td.reqUserLogin({ brokerId: "9999", userId: "id", password: "pw" });
await td.reqSettlementInfoConfirm({ brokerId: "9999", investorId: "id" });

// the settlement statement text
const [info] = await td.reqQrySettlementInfo({ brokerId: "9999", investorId: "id" });
```

Common ones: `reqAuthenticate`, `reqUserLogin`, `reqUserLogout`,
`reqSettlementInfoConfirm`, `reqQrySettlementInfo`, `reqQryInstrument`,
`reqQryInvestorPosition`, `reqQryInvestorPositionDetail`, `reqQryTradingAccount`,
`reqQryOrder`, `reqQryTrade`, `reqOrderInsert`, `reqOrderAction`. (The full set
is generated from the CTP headers — 111 request methods.)

---

## Types

### `RiskConfig`

Pre-trade risk limits for [`riskSet`](#tdrisksetconfig--this). All fields
optional; **omit or `0` = disabled**; `NaN`/`Infinity` throws.

| Field | Type | Meaning |
|---|---|---|
| `maxOrderVolume` | `number` | Max lots per single order. |
| `maxPriceDeviation` | `number` | Max `\|price − reference\| / reference` ratio (e.g. `0.02` = 2%); needs a reference via [`trackMarketData`](#tdtrackmarketdatamd--this). |
| `maxNotional` | `number` | Max notional (price × volume × multiplier) per order. |
| `maxOrdersPerSec` | `number` | Max order sends per second (token bucket). |
| `orderBurst` | `number` | Token-bucket burst size. Default: `maxOrdersPerSec`. |
| `maxMargin` | `number` | Cap on total open-position **margin** = Σ(price × volume × multiplier × marginRate) across the whole book — the real capital occupied. Enforced in C++ on every opening order (including armed orders). Margin rates are fed automatically from CTP (query `reqQryInstrumentMarginRate` to populate; until a contract's rate is known it counts at full notional = conservative). |
| `maxPositionCost` | `number` | **Deprecated** alias for `maxMargin` (also margin-based now). `maxMargin` takes precedence when both are set. |

```ts
const conservative: RiskConfig = { maxOrderVolume: 5, maxOrdersPerSec: 10, maxNotional: 2_000_000 };
td.riskSet(conservative);
```

### `LotCap`

`number | { long?: number; short?: number }` — the cap argument to
[`setMaxPosition`](#tdsetmaxpositioninstrumentid-max--tdsetmaxpositionslimits--this).
A number caps both sides; `{ long, short }` caps each (omit a side = unchanged,
`≤ 0` = clear).

```ts
const a: LotCap = 100;                  // both sides ≤ 100
const b: LotCap = { long: 50, short: 5 }; // long ≤ 50, short ≤ 5
```

### `SessionOptions`

Options for [`session`](#tdsessionopts--promise-multipliers-positions-orders-).
See the table there.

```ts
const opts: SessionOptions = {
  brokerId: "9999", userId: "id", password: "pw",
  appId: "simnow_client_test", authCode: "0000000000000000",
  confirmSettlement: true,
  sync: { multipliers: ["rb2510"], positions: true, orders: true },
};
```

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

```ts
const spec: ArmSpec = {
  instrumentId: "rb2510", side: "sell", triggerPrice: 3450,
  order: { instrumentId: "rb2510", direction: "1", combOffsetFlag: "3", limitPrice: 3445, volumeTotalOriginal: 1 },
};
const handle = td.arm(md, spec);
```

### `ArmHandle`

```ts
interface ArmHandle {
  readonly id: number;
  disarm(): boolean; // remove the trigger; false if it was already gone/fired
}
```

```ts
const handle = td.arm(md, spec);
console.log("armed id", handle.id);
if (!handle.disarm()) console.log("it had already fired");
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

```ts
td.on("rsp-qry-investor-position", (row, options) => {
  if (options.rspInfo) return console.error(options.rspInfo.errorMsg);
  collect(row);
  if (options.isLast) finish(); // final row of the multi-row response
});
```

### `RspInfo`

```ts
interface RspInfo {
  errorId: number;      // 0 = success
  errorMsg: string;     // human-readable (GB18030-decoded)
}
```

A rejected request Promise's error carries these fields too
(`err.errorId` / `err.errorMsg`):

```ts
try { await td.reqUserLogin({ brokerId: "9999", userId: "id", password: "bad" }); }
catch (e) { console.error(`CTP ${e.errorId}: ${e.errorMsg}`); }
```

### `MdLoginReq`

```ts
interface MdLoginReq {
  brokerId?: string; userId?: string; password?: string;
  tradingDay?: string; userProductInfo?: string;
}
```

```ts
const req: MdLoginReq = { brokerId: "9999", userId: "id", password: "pw" };
await md.login(req);
```

---

## Enums & struct types

All CTP enums (values) and struct interfaces (types) are generated from the CTP
headers and re-exported from the package root.

**Enums** are string-valued and usable both as the enum member and the raw code:

```ts
import { Direction, OffsetFlag, OrderStatus, OrderPriceType, ActionFlag } from "@hitrading/ctp-node";

Direction.Buy;          // "0"
Direction.Sell;         // "1"
OffsetFlag.Open;        // "0"
OffsetFlag.CloseToday;  // "3"
OrderStatus.AllTraded;  // "0"
ActionFlag.Delete;      // "0"

// both forms type-check on a struct field:
const a = { direction: Direction.Buy };
const b = { direction: "0" } as const;
```

**Struct types** describe the shape of event payloads and request fields — e.g.
`DepthMarketData` (a tick), `Order`, `Trade`, `InputOrder`,
`InvestorPosition`, `RspUserLogin`. Import them as types:

```ts
import type { DepthMarketData, Order, Trade, InputOrder } from "@hitrading/ctp-node";

md.on("rtn-depth-market-data", (tick: DepthMarketData) => {
  const mid = (tick.bidPrice1 + tick.askPrice1) / 2;
});

function buildOrder(): Partial<InputOrder> {
  return { instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 1 };
}
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
| Max position margin (per instrument) | `setMaxPositionCost(id, …)` | Per instrument | `≤ 0` |
| Max position margin (book) | `riskSet({ maxMargin })` | Whole book | `0` / omitted |

Position-lot and position-margin caps count **committed = held (filled) +
in-flight (working order) volume**, so a burst of opens can't slip past a cap
before the fills report. Feed held positions via `syncPositions()` /
`seedPosition()` and working orders via `syncOrders()` (especially after a
reconnect). Margin rates are fed automatically from CTP (query
`reqQryInstrumentMarginRate`); until a contract's rate is known it counts at full
notional (conservative). All caps are enforced in C++ on the send path; a breach
makes `reqOrderInsert()` reject with the reason in the message.

---

## Recipes

End-to-end patterns that stitch the individual calls above into something
runnable.

### A complete strategy skeleton

A minimal but production-shaped skeleton: risk first, handshake on connect,
positions tracked from fills (the source of truth), graceful shutdown. Drop your
own logic into `signal()`.

```ts
import {
  MarketData, Trader, Direction, OffsetFlag, type DepthMarketData, type Trade,
} from "@hitrading/ctp-node";

const CREDS = {
  brokerId: "9999", userId: "your-id", password: "your-pw",
  appId: "simnow_client_test", authCode: "0000000000000000",
};
const SYMBOL = "rb2510";

const md = new MarketData("./flow/md/", "tcp://182.254.243.31:30012");
const td = new Trader("./flow/td/", "tcp://182.254.243.31:30002");

// 1) Configure risk BEFORE any order can be sent (enforced in C++).
td.riskSet({ maxOrderVolume: 5, maxNotional: 2_000_000, maxOrdersPerSec: 10, maxPriceDeviation: 0.02, maxMargin: 5_000_000 });
td.setMaxPosition(SYMBOL, { long: 10, short: 10 });
td.trackMarketData(md);              // live reference price for the deviation check

// 2) Never let a buggy handler wedge the feed — log handler throws.
md.on("error", (e) => console.error("[md handler]", e));
td.on("error", (e) => console.error("[td handler]", e));

// 3) Handshake on connect AND every reconnect (CTP reconnects on its own).
td.on("front-connected", async () => {
  try { await td.session({ ...CREDS }); console.log("trader ready"); }
  catch (e: any) { console.error("session failed", e.errorId, e.errorMsg); }
});
md.on("front-connected", async () => {
  await md.login();                  // SimNow MD front accepts anonymous login
  md.subscribe([SYMBOL]);
});

// 4) Position is derived from FILLS, never assumed from sends.
let position = 0; // net lots (+long / -short)
td.on("rtn-trade", (t: Trade) => {
  position += t.direction === Direction.Buy ? t.volume : -t.volume;
  console.log("fill", t.price, "x", t.volume, "-> net position", position);
});

// 5) The strategy: turn each tick into a target and trade the difference.
md.on("rtn-depth-market-data", async (tick: DepthMarketData) => {
  if (tick.instrumentId !== SYMBOL) return;
  const want = signal(tick);         // your logic: target net lots, e.g. -1 / 0 / +1
  const delta = want - position;
  if (delta === 0) return;
  const buy = delta > 0;
  await td.reqOrderInsert({
    instrumentId: SYMBOL,
    direction: buy ? Direction.Buy : Direction.Sell,
    // open if we're growing the position, close if we're shrinking it
    combOffsetFlag: Math.sign(want) === Math.sign(position) || position === 0 ? OffsetFlag.Open : OffsetFlag.CloseToday,
    limitPrice: buy ? tick.askPrice1 : tick.bidPrice1, // cross the spread to fill
    volumeTotalOriginal: Math.abs(delta),
  }).catch((e) => console.warn("order refused:", e.message));
});

function signal(_tick: DepthMarketData): number { return 0; /* TODO your alpha */ }

// 6) Graceful shutdown — halt, then close ONCE.
for (const sig of ["SIGINT", "SIGTERM"] as const) {
  process.on(sig, () => { td.halt(); td.close(); md.close(); process.exit(0); });
}
```

### Robust reconnect handling

CTP reconnects on its own; you re-run the handshake **in place** on every
`front-connected` — never `new Trader()` to reconnect (the vendor DLL deadlocks;
see [native-hooks.md](native-hooks.md#process-lifecycle-create-once-reuse)).

```ts
td.on("front-connected", async () => {
  // session() re-authenticates, re-logs-in, re-confirms, and crucially re-syncs
  // multipliers/positions/orders — so the risk engine's position caps are rebuilt
  // to include orders that were working before the drop (and any from another
  // terminal on the same account). Then re-arm whatever you need.
  await td.session({ ...CREDS });
  rearmTriggers();
});

td.on("front-disconnected", (reason) => {
  console.warn("trader link down (code", reason, ") — CTP will auto-reconnect");
  // pending request Promises have already rejected; pause new signals until
  // the next front-connected if your strategy needs a confirmed session.
});

function rearmTriggers() { /* re-create any td.arm(...) triggers here */ }
```

If you prefer to hand-roll instead of `session()`, the key is to **re-sync orders
after a reconnect** so in-flight reservations are rebuilt:

```ts
td.on("front-connected", async () => {
  await td.reqUserLogin({ ...CREDS });
  await td.syncMultipliers();   // before syncOrders, so reserved cost uses the right multiplier
  await td.syncPositions();     // rebuild held-position cost
  await td.syncOrders();        // rebuild in-flight reservations from working orders
});
```

### Combination & multi-leg orders

**Close-today vs close-yesterday.** SHFE/INE distinguish them, so `"close"` isn't
enough — pick `CloseToday` (`"3"`) or `CloseYesterday` (`"4"`). To flatten 5 long
lots that are 2 opened today + 3 from prior days, send two orders:

```ts
import { Direction, OffsetFlag } from "@hitrading/ctp-node";

await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseToday,     limitPrice: px, volumeTotalOriginal: 2 });
await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseYesterday, limitPrice: px, volumeTotalOriginal: 3 });
```

**Spread / combination instruments (multi-leg `combOffsetFlag`).**
`combOffsetFlag` is **one offset char per leg**. For an exchange-listed
combination instrument (e.g. a SHFE calendar spread `SP rb2510&rb2601`, two
legs), pass a 2-char flag — here open both legs (`"0"` + `"0"` → `"00"`):

```ts
await td.reqOrderInsert({
  instrumentId: "SP rb2510&rb2601", // exchange-specific combo id format
  direction: Direction.Buy,
  combOffsetFlag: "00",             // leg1 open, leg2 open
  limitPrice: spreadPrice,
  volumeTotalOriginal: 1,
});
// close-today the near leg, open the far leg of a 2-leg combo: "30"
```

**Building/dissolving a combined position** (where the exchange supports it) goes
through `reqCombActionInsert` with an `InputCombAction`:

```ts
await td.reqCombActionInsert({
  brokerId: "9999", investorId: "your-id",
  instrumentId: "SPC m2509&m2601",  // the combination instrument
  combDirection: "0",               // 0 = build the combination, 1 = split it
  volume: 1,
  // ...remaining InputCombAction fields per your exchange
});
```

> Multi-leg flags and combination instruments are **exchange-specific** — the id
> format and which combos exist vary by exchange. The pre-trade risk gate's
> per-instrument caps key on the combo instrument id, not the underlying legs.
