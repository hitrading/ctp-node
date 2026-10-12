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
- [Types](#types) — `RiskLimits`, `InstrumentPositionLimit`, `SessionOptions`, `ArmSpec`, `ArmHandle`, `CallbackOptions`, `RspInfo`, `MdLoginReq`
- [Enums & struct types](#enums--struct-types)
- [Risk controls at a glance](#risk-controls-at-a-glance)
- [Recipes](#recipes) — strategy skeleton · reconnect handling · combination orders

```ts
import {
  MarketData, Trader,
  Direction, OffsetFlag, OrderPriceType, // generated enums
  type RiskLimits, type SessionOptions, type DepthMarketData,
} from "@hitrading/ctp-node";
```

---

## Install

```bash
npm install @hitrading/ctp-node
```

Supported Node.js release lines are `^22.22.2 || ^24.15.0 || >=26.0.0`, matching
the bundled node-gyp 13 requirement. Prebuilt binaries ship in the npm package
for win32-x64 and linux-x64. macOS and other platforms build from source on
install (a C++ toolchain + Python are required).

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

td.setRiskLimits({ maxOrderVolume: 10, maxOrdersPerSec: 20 });
td.setInstrumentPositionLimit("rb2510", 100); // never hold more than 100 lots/side

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
Response-event payloads may be `undefined` on an error or an empty final row;
inspect `options.rspInfo` / `options.isLast` before dereferencing them.

**Strict request objects.** Requests/configs must be object literals (or
null-prototype dictionaries) with explicit own fields. Unknown, inherited,
misspelled, removed, symbol, and `null` field values are rejected before a
request reaches CTP; they are never silently encoded as zero. Input arrays must
be dense and contain explicit own entries; sparse or prototype-provided values
are rejected.

**Create once, reuse for the life of the process.** Construct a `MarketData` /
`Trader` once and keep it. CTP reconnects on its own — handle a dropped link
**in place** by re-running your handshake when `front-connected` fires again (it
fires on the first connect *and* every auto-reconnect). Do **not** destroy and
recreate the client: repeated Windows create/close cycles have been observed to
deadlock inside the vendor DLL's `Release()`. Call `close()` once at shutdown (or not at all —
process exit cleans up). See
[native-hooks.md → Process lifecycle](native-hooks.md#process-lifecycle-create-once-reuse).

**Backpressure.** Each client decodes ticks/returns from a native ring. Market
data uses bounded keep-newest overflow staging and retains the freshest quote.
Trader overflow is lossless and ordered: it spills beyond the
4096-slot ring instead of dropping callbacks. Sustained Trader overload can use
unbounded memory, so event handlers still need to remain bounded.

**Pre-trade risk runs in C++.** All risk limits (`setRiskLimits`,
`setInstrumentPositionLimit`, `reduceOnly` / `haltAll`, …) are enforced on the order-send path inside the native addon, before
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

### `md.login(req?)` → `Promise<RspUserLogin | undefined>`

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
>
> Await every market-data `login()` / `logout()` before starting another one.
> Some fronts answer these requests with `requestId = 0`, so the binding
> deliberately permits only one such transition at a time. If one times out,
> the native cache/armed-order path remains fail-closed: the first late final
> response is ignored (or a front transition resets the state) before a new
> authenticated cache epoch can be established.

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

### `md.logout(req?)` → `Promise<UserLogout | undefined>`

Log out. `req` may carry `{ brokerId?, userId? }`.

As with `login()`, market-data login/logout transitions must not overlap; await
the returned Promise before starting the next one.

```ts
await md.logout();
// or scoped to a specific account
await md.logout({ brokerId: "9999", userId: "id" });
```

### `md.reqQryMulticastInstrument(req?)` → `Promise<MulticastInstrument[]>`

Query multicast instruments and resolve all response rows as an array. The
request may contain `topicId` and `instrumentId`. Calls use the same conservative
per-client query scheduler as other CTP queries, with at least 1100 ms between
sends. No caller-side sleep is required.

```ts
const instruments = await md.reqQryMulticastInstrument({ topicId: 1 });
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
C++ last-value cache. A successful MD login starts a new, empty cache epoch;
only subsequent ticks in that authenticated epoch update it before reaching JS.
Connect/disconnect, a new login/logout attempt, successful logout, and close
invalidate and clear the epoch immediately. Returns `null` outside an active
epoch, if no tick has been seen for that instrument, or after unsubscribe.

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
**synchronously**. Returns `0` outside an authenticated cache epoch, if no tick
has been seen, or after the entry is cleared — see
[`snapshot`](#mdsnapshotinstrumentid--depthmarketdata--null).

```ts
const px = md.last("rb2510");
if (px > 0) console.log("last traded", px);
```

### `md.attachArm(trader)` → `void`

Route this feed's ticks to a `Trader`'s armed triggers (see
[`Trader.arm`](#tdarmmd-spec--armhandle)). Usually you call `td.arm(md, …)`,
which calls this for you. A `MarketData` feeds exactly one **live** `Trader`'s
triggers; attaching a second live Trader throws. After the old Trader has closed,
its registry may be replaced without retaining the closed JS object.

```ts
// explicit (rarely needed — td.arm() does this automatically):
md.attachArm(td);

// a second, different live trader throws:
md.attachArm(td);        // ok
md.attachArm(otherTd);   // throws: already feeds another Trader
```

### `md.apiVersion` / `md.tradingDay` → `string` (read-only)

The CTP API version string, and the current trading day (`YYYYMMDD`), available
after connect.

```ts
console.log("CTP MD API", md.apiVersion); // e.g. "6.7.2"
md.on("front-connected", () => console.log("trading day", md.tradingDay));
```

### `md.droppedRecords` → `number` (getter)

Total ticks discarded by keep-newest backpressure. Monitor it to detect a slow
consumer.

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
| `rsp-qry-multicast-instrument` | `MulticastInstrument` | One multicast-instrument query row. |
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

### `td.session(opts)` → `Promise<{ identity, multipliers, marginRates, positions, orders }>`

One-call post-connect handshake: **authenticate → login → confirm settlement →
sync multipliers / optional margin rates / orders / positions**. Call it from the `front-connected`
handler (and again after a reconnect). Returns the request-correlated
`identity: { frontId, sessionId, maxOrderRef }` plus row counts from the sync.
Here `maxOrderRef` is the value reported by that login response; the Trader's
automatic allocator may retain a higher process-local high-water mark.
Each step just wraps the matching request/sync method, so you can
hand-roll the flow when you need finer control.

At handshake start, held-position and working-order ledgers are marked
uncertain, so all opening inserts fail closed across a possible
reconnect gap. Successful `syncPositions()` and `syncOrders()` each clear only
their own uncertainty. Disabling either sync leaves that side fail-closed until
you reconcile it later.

A newly created Trader starts in the same fail-closed state, including the small
window before its first `front-connected` callback.

`opts` is a [`SessionOptions`](#sessionoptions):

The login response must contain a concrete `frontId` / `sessionId` and the same
`brokerId` / `userId` that were requested. An empty or mismatched success
payload is rejected because those fields are part of the local reservation and
snapshot identity; reusing an old/zero session or reconciling another account
would make the ledger unsafe.

| Field | Type | Meaning |
|---|---|---|
| `brokerId` | `string` | Broker id (required). |
| `userId` | `string` | Investor id (required). |
| `password` | `string` | Password (required). |
| `appId` | `string?` | Terminal app id for authentication. SimNow: `"simnow_client_test"`. |
| `authCode` | `string?` | Terminal auth code. SimNow: 16 zeros. Omit both `appId`/`authCode` to skip authentication. |
| `confirmSettlement` | `boolean?` | Confirm the settlement statement after login (real accounts must, or orders are rejected). Default `true`. |
| `sync` | `object?` | `{ multipliers?: boolean \| string[], marginRates?: { instrumentIds: string[], hedgeFlag: "1" \| "2" \| "3" \| "5" \| "6" \| "7" }, positions?: boolean, orders?: boolean }`. Multipliers/positions/orders default on; margin rates require an explicit bounded contract list and hedge flag because CTP needs one query per contract and keeps separate schedules per hedge flag. |

```ts
// SimNow full handshake
td.on("front-connected", async () => {
  const counts = await td.session({
    brokerId: "9999", userId: "id", password: "pw",
    appId: "simnow_client_test", authCode: "0000000000000000",
  });
  console.log("synced", counts); // { identity, multipliers, marginRates, positions, orders }
});

// scope the multiplier sync to the symbols you trade (faster cold start)
await td.session({
  brokerId: "9999", userId: "id", password: "pw",
  sync: {
    multipliers: ["rb2510", "au2508"],
    marginRates: { instrumentIds: ["rb2510", "au2508"], hedgeFlag: "2" },
    positions: true,
    orders: true,
  },
});

// skip the settlement confirm (environments that don't need it)
await td.session({ brokerId: "9999", userId: "id", password: "pw", confirmSettlement: false });
```

> The hand-rolled equivalent: `await td.reqAuthenticate(...)` →
> `await td.reqUserLogin(...)` → `await td.reqSettlementInfoConfirm(...)` →
> `await td.syncMultipliers()` → `await td.syncMarginRates([...])` → `await td.syncOrders()` →
> `await td.syncPositions()`.

### `td.reqOrderInsert(req?)` → `Promise<void>`

Insert an order through the C++ pre-trade risk gate. **Resolves on submission** —
CTP sends no success acknowledgement for an accepted order, only `rtn-order` /
`rtn-trade` (correlate by `orderRef`). It **rejects** only if the send is refused
(risk gate, rate limit, or a CTP API error code). A blank `orderRef` is assigned
a unique numeric value automatically.

A confirmed login is required. `brokerId` and `investorId` are filled from that
session; supplying a different value is rejected before encoding, so the local
position/order snapshots cannot be applied to an order for another account.

`req` is a `Partial<InputOrder>`; the fields you almost always set:

| Field | Type | Meaning |
|---|---|---|
| `instrumentId` | `string` | Contract, e.g. `"rb2510"`. |
| `direction` | `Direction` (`"0"`/`"1"`) | `Buy` / `Sell`. |
| `combOffsetFlag` | `string` | Exactly one offset char: `"0"` open, `"1"` close, `"3"` close-today, … Multi-leg flags are rejected. |
| `limitPrice` | `number` | Limit price; must be `> 0`. Market / any-price orders (`limitPrice: 0`) are **not** supported. |
| `volumeTotalOriginal` | `number` | Quantity in lots (positive int32). |
| `orderRef` | `string?` | Leave unset for an auto-assigned unique ref; otherwise 1–12 decimal digits. |
| `orderPriceType` | `OrderPriceType?` | Optional; if set, must be `"2"` (limit). Defaults to limit. |

> **Only single-instrument GFD, FAK and FOK limit orders are accepted.** Use `timeCondition: "3", volumeCondition: "1"` for GFD; `"1", "1"` for FAK; and `"1", "3"` for FOK. `reqOrderInsert` deliberately rejects (with a `TypeError`) anything the native risk-reservation ledger cannot represent: other time/volume-condition combinations, min-volume/conditional/stop/swap/force-close variants, and multi-leg offset/hedge flags.

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
await td.reqOrderInsert({ orderRef: "42001", instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 2 });
td.on("rtn-trade", (t) => { if (t.orderRef === "42001") console.log("my order filled", t.volume); });

// 4) close-today a short position (sell -> close)
await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseToday, limitPrice: 3490, volumeTotalOriginal: 1 });
```

### `td.reqOrderAction(req?)` → `Promise<void>` (cancel)

Cancel / modify a working order. Resolves on submission. A Delete action remains
available in every risk mode. A Modify action can increase exposure, so it is
treated as an unsupported mutation and is refused unless the Trader is in
`NORMAL` with every local risk control disabled. Identify the order via
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

### Query methods — `td.reqQry*` / `td.reqQuery*`

Every `reqQry*` resolves with an **array of rows** (multi-row responses are
accumulated; an empty result resolves `[]`); the two single-response `reqQuery*`
methods resolve one row. CTP rate-limits queries to roughly one per second.
Every generated query shares a per-Trader scheduler with at least 1100 ms
between attempted sends, including concurrent direct calls; it keeps at most one
query in flight, so callers do not add `setTimeout(1000)`. Request data is
snapshotted when the method is called, and disconnect/login/logout transitions
reject queued work from the old context. The scheduler is per client (not
process-global) and accepts at most 256 active/queued queries. Raw queries are
paced but not retried. The
[`sync*`](#tdsyncmultipliersinstrumentids--promisenumber) helpers additionally
retry transient `-2`/`-3` flow-control failures and required empty cold-start
data.

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
symbol list to scope it. An explicit empty list is a no-op returning `0`; only an
omitted argument means "query all". Retries through cold-start flow control.
Returns the count of contracts whose final callback row has a usable multiplier.

Starting an instrument refresh invalidates the old multiplier before the request
is sent, so a failed/empty refresh cannot leave stale metadata active. If held
positions exist, run `syncPositions()` afterwards to refresh their authoritative
`UseMargin`; monetary opening gates remain fail-closed until then.

```ts
const n = await td.syncMultipliers();          // all instruments
await td.syncMultipliers(["rb2510", "au2508"]); // just these
console.log(n, "contracts with a multiplier");
```

### `td.syncMarginRates(instrumentIds, hedgeFlag?)` → `Promise<number>`

Investor margin queries are grouped by the ProductID in a fresh complete CTP catalog. Contract overrides take precedence even when invalid. C++ expands the complete successful response atomically into contract risk schedules. Relative schedules still require per-contract exchange bases. `readMarginRates(instrumentIds, hedgeFlag)` returns copies of completed synchronization results from the current session only; incomplete queries, reconnects and product refreshes invalidate old snapshots.

Query investor long/short by-money/by-volume schedules for a bounded contract
universe (default hedge flag `"1"`, speculation). A second exchange-base query
is issued only for a relative row; the shared scheduler spaces every query.
Relative investor schedules are added to their exchange base. Missing metadata
fails closed.

Old investor and exchange schedules are invalidated before the refresh. Run
`syncPositions()` after refreshing schedules when the account already holds
positions; the engine will not treat margin estimated under an old schedule as
authoritative.

### `td.syncPositions()` → `Promise<number>`

Seed open-position **margin** from CTP (`reqQryInvestorPosition`, each row's
`UseMargin`) into the risk engine's position tracker. It always uses the
confirmed login identity; account overrides are rejected. Returns the number of
positions seeded. Empty snapshots must be observed twice; applying the snapshot is an
atomic revision-checked replacement that preserves working reservations. A
realtime callback during the query causes a retry rather than a clear/reseed gap.

```ts
const held = await td.syncPositions();
console.log("seeded", held, "positions, margin =", td.accountMargin);
```

### `td.syncOrders()` → `Promise<number>`

Rebuild the in-flight reservation from CTP's currently-working orders
(`reqQryOrder`) — an authoritative resync. Call after login and after any
reconnect so the position caps account for orders already working at the broker
(placed before a reconnect, or — since CTP delivers them too — from another
terminal on the same account). Run `syncMultipliers()` first so the reserved cost
uses the right multiplier and query margin schedules before monetary caps are
enabled. Conditional statuses (`a`/`b`/`c`) remain reserved. Applying the
snapshot is revision-checked against realtime callbacks. Returns the number of
working open orders re-reserved.

If the account has a working multi-leg/unsupported order (including one placed
by another terminal), its exposure cannot be represented by this single-leg
ledger. The snapshot is still committed atomically, that order is recorded as
unsupported, and the method throws. Opening inserts then fail
closed until a later successful `syncOrders()` confirms the order is gone.

`syncOrders()` deliberately marks the held-position side uncertain too. Run
`syncPositions()` **after it**; reversing the order stays fail-closed instead of
opening on two broker snapshots that have no transactional common cut.

```ts
await td.syncMultipliers();
const working = await td.syncOrders();
await td.syncPositions();
console.log("re-reserved", working, "working open orders");
```

### Risk configuration

#### `td.setRiskLimits(config)` → `this`

Publish pre-trade risk limits to the C++ enforcer (takes effect at once). A
non-finite (`NaN`/`Infinity`) limit is **rejected with a throw** rather than
silently disabling the control. `config` is a [`RiskLimits`](#risklimits); omit a
field or pass `0` to disable that control; negative values are rejected. The object replaces the
whole prior configuration; `maxReferenceAgeMs` is the exception: it must be
positive and defaults to 3000 ms when omitted. Unknown fields (including removed
legacy names such as `maxMargin`) are rejected instead of being ignored.

```ts
// full set
td.setRiskLimits({
  maxOrderVolume: 10,          // ≤ 10 lots per order
  maxPriceDeviation: 0.02,     // ≤ 2% from the reference price
  maxNotional: 5_000_000,      // ≤ 5M Futures/TAS notional per opening order
  maxOrdersPerSec: 20,         // token-bucket rate limit
  orderBurst: 40,              // bucket size (default: maxOrdersPerSec)
  maxCloseOrdersPerSec: 10,    // optional independent close-order rate
  closeOrderBurst: 20,
  maxAccountMargin: 20_000_000, // ≤ 20M held + reserved account margin
});

// just a couple of controls (the rest stay disabled)
td.setRiskLimits({ maxOrderVolume: 5, maxOrdersPerSec: 10 });

// disable a control by passing 0
td.setRiskLimits({ maxNotional: 0 });
```

#### `td.reduceOnly()` / `td.haltAll()` / `td.resume()` → `this`

The atomic modes are `NORMAL`, `REDUCE_ONLY`, and `HALT_ALL`. `reduceOnly()`
blocks normal opens, every advanced mutation, and bank/futures transfers while
keeping cancellations and single-leg normal closes available. `haltAll()` also
blocks close inserts, clears all armed triggers, and rejects new `arm()` calls
until `resume()`. Mode transitions share the
final vendor-API send barrier, so no forbidden armed order can cross after the
transition returns. `resume()` restores `NORMAL`.

Advanced mutations (complex inserts, offset setting, spread/hedge confirmation)
cannot be represented by the normal reservation ledger, so they fail closed
whenever any local risk control is configured. The generator requires every
future non-query SDK request to be classified; an unreviewed method fails code
generation instead of defaulting to unguarded.

This separation is lifetime-sticky: after CTP accepts any unsupported risk
mutation in pure binding mode, later enabling a local control makes ordinary
opening orders fail closed. `syncOrders()` covers normal single-leg orders only
and therefore cannot certify that quote/exec/comb/parked exposure has gone away.
Realtime advanced-mutation and funds-transfer returns observed from another
terminal set the same sticky fail-closed marker; they are not mistaken for
ordinary-ledger state merely because this process did not originate them.
Use a Trader instance in one capability mode for its whole lifetime; do not
switch a protocol-binding session into locally managed trading after advanced
mutations have been admitted.

Bank/futures transfers are subject to the same rule. They change available
collateral but are not represented by this local ledger, so they are forwarded
only in `NORMAL` mode when every local risk control is disabled. Authentication
and transfer authorization remain the caller's responsibility.

```ts
// stop risk increases but keep closes/cancels working
process.on("SIGINT", () => td.reduceOnly());

// stop both opens and closes (cancellations still work)
td.haltAll();

// re-enable trading once you've assessed the situation
td.resume();

// example: halt if PnL breaches a limit
td.on("rtn-trade", () => { if (computePnl() < -100000) td.reduceOnly(); });
```

#### `td.setInstrumentPositionLimit(instrumentId, limit)` / `td.setInstrumentPositionLimits(limits)` → `this`

Cap the open position (in lots) per instrument, enforced on every opening order.
`limit` is an [`InstrumentPositionLimit`](#instrumentpositionlimit): a number caps both sides; `{ long, short }` caps
each side separately. Within `{ long, short }`, **omitting a side leaves its
current cap unchanged** (pass `0` to clear; negative values are rejected). Long and short are tracked
independently. The check counts committed = held + in-flight (working) volume.

```ts
td.setInstrumentPositionLimit("rb2510", 100);                  // long ≤ 100 and short ≤ 100
td.setInstrumentPositionLimit("au2508", { long: 50, short: 10 }); // asymmetric

// raise only the long cap, leave short as it was
td.setInstrumentPositionLimit("au2508", { long: 80 });
// clear the short cap
td.setInstrumentPositionLimit("au2508", { short: 0 });

// many at once
td.setInstrumentPositionLimits({ rb2510: 100, ru2510: { long: 100, short: 20 }, au2508: 10 });
```

#### `td.setInstrumentMarginLimit(instrumentId, limit)` / `td.setInstrumentMarginLimits(limits)` → `this`

Cap one instrument's open-position **margin** (price × volume × multiplier ×
by-money + volume × by-volume, long and short summed).
Per-instrument and independent of the account-wide
`setRiskLimits({ maxAccountMargin })`; both apply. Pass `limit = 0` to remove it; negative values are rejected. Until a
contract multiplier and usable absolute margin schedule are known (fed from CTP
callbacks after the corresponding queries), capped opening orders fail closed.
Both by-money and by-volume/per-lot components are included. `IsRelative`
schedules are added to the matching exchange schedule loaded by
`syncMarginRates()`; either half missing fails closed.

This formula is enabled only for CTP Futures/TAS product classes. Options,
combinations and spot products require product-specific margin models and fail
closed while a monetary cap is active.

`maxNotional` uses the same Futures/TAS allowlist. Option premium and combination
price are not treated as futures notional; unknown/unsupported product metadata
fails closed when the notional guard is enabled.

Notional and working-order margin are valued at the greater of the order price
and a fresh market reference. This prevents a low sell/open limit from
understating exposure. Attach market data with `trackMarketData()`; missing or
older-than-`maxReferenceAgeMs` quotes make every enabled monetary gate fail
closed. Existing working orders are revalued before each monetary admission.

```ts
td.setInstrumentMarginLimit("au2508", 5_000_000); // ≤ 5M of gold margin
td.setInstrumentMarginLimits({ ag2508: 2_000_000, au2508: 5_000_000 });
td.setInstrumentMarginLimit("au2508", 0);         // remove the cap
```

#### `td.trackMarketData(md)` → `this`

Provide the live reference price used by price deviation, notional and
working-order margin checks.
Wires the `MarketData` feed's C++ snapshot cache straight into this Trader's risk
engine, so the deviation reference is read **in C++ on the order-send path** — no
JS round-trip, and it covers armed orders that fire from C++ with no JS in the
loop. Without a reference, or when its age exceeds `maxReferenceAgeMs` (default
3000 ms), every enabled deviation or monetary check fails closed for opening
orders.

```ts
td.setRiskLimits({ maxPriceDeviation: 0.02 });

// live reference from the feed (read in C++ on every order, incl. armed orders)
td.trackMarketData(md);
```

> Contract multipliers (for notional / cost accounting) and per-contract margin
> rates (for the `maxAccountMargin` cap) are **not** fed from JS: the C++ risk engine
> sources them directly from CTP's `OnRspQryInstrument` /
> `OnRspQryInstrumentMarginRate` callbacks. So any `reqQryInstrument` keeps
> multipliers current (see [`syncMultipliers`](#tdsyncmultipliersinstrumentids--promisenumber)),
> Prefer `syncMarginRates()` for every contract/hedge flag you trade: it queries
> both investor and exchange schedules, includes both formula components, and
> combines `IsRelative` correctly. Missing metadata is never guessed.

### Position-margin tracking

#### `td.seedPositions(positions)` → `this`

Atomically replace held positions from a **complete** manually reconciled
snapshot. Each row is `{ instrumentId, side, hedgeFlag, volume, margin }`, where
`margin` is the actual capital occupied (`InvestorPosition.UseMargin`). An empty
array is an authoritative flat book. The removed singular `seedPosition()` is
intentionally not retained: several incremental JS calls would expose a partial
book to native armed orders between calls.

```ts
td.seedPositions([
  { instrumentId: "rb2510", side: "long", hedgeFlag: "1", volume: 3, margin: 30_000 },
  { instrumentId: "au2508", side: "short", hedgeFlag: "1", volume: 2, margin: 112_000 },
]);
```

#### `td.seedFromPositions(positions)` → `this`

Atomically replace held open-position **margin** from a complete set of
`reqQryInvestorPosition` rows (each row's `UseMargin`) for **gross-mode**
accounts (`posiDirection` `"2"` = long, `"3"` = short), retaining each
supported `HedgeFlag` as a separate cost bucket. Net/unknown directions or
hedge flags are rejected because guessing would corrupt per-side volume or
margin accounting; convert those rows explicitly and submit one complete
`seedPositions()` snapshot.

```ts
const rows = await td.reqQryInvestorPosition({ brokerId: "9999", investorId: "id" });
td.seedFromPositions(rows); // complete held snapshot; working reservations remain
```

#### `td.accountMargin` → `number` (read-only)

Current held-position margin tracked by the risk engine. It deliberately excludes
working-order reservations, although `maxAccountMargin` admits against
**held + reserved** margin. This property is best-effort observability: when
metadata is missing or refreshed it may not be an exact admission value; an
enabled monetary gate still fails closed until an authoritative position sync.

```ts
console.log("book margin", td.accountMargin);
td.on("rtn-trade", () => console.log("margin now", td.accountMargin));
```

#### `td.resetPositions()` → `this`

Clear held position margin only. Working-order reservations are deliberately
preserved. A reset is not proof that the account is flat, so opening inserts
remain fail-closed until `seedFromPositions()` or `syncPositions()` completes an
authoritative revision-checked replacement.

```ts
td.resetPositions();              // clear held rows, keep reservations; block opens
td.seedFromPositions(freshRows);  // atomic complete replacement; reopen the ledger
```

### Latency-critical armed triggers

#### `td.arm(md, spec)` → `ArmHandle`

Arm a trigger evaluated and fired entirely in C++ on the market-data callback
thread — when `md` sees the condition, the order is sent through this Trader's
risk gate with **no JS round trip**. It is removed after a successful submission;
a halt/rate/metadata/send refusal leaves it armed with exponential retry
backoff (100 ms initially, capped at 5 seconds).
`haltAll()` is the hard-stop exception: it closes the final send barrier and
clears every existing arm, so `resume()` cannot fire a historical trigger.
Front connect/disconnect events and every Trader login/logout request also
clear all arms: a mandate never crosses an authenticated session boundary.
Recreate the required triggers only after `session()` has reconciled the new
session.
The acknowledgement arrives via the normal `rtn-order` / `rsp-order-insert`
events (correlate by `orderRef`).

The supplied `md` is also attached automatically as this Trader's fresh-price
source for deviation, notional and working-margin checks.

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

#### `td.armStats` → `{ fired: number; blocked: number }` (read-only)

Observability for armed triggers (they fire in C++ with no JS in the loop):
how many fired and were sent (`fired`) vs were refused by the risk gate / send
(`blocked`). Poll this after a trigger you expected to fire — a blocked armed
order is otherwise invisible.

```ts
const { fired, blocked } = td.armStats;
console.log(`armed: ${fired} sent, ${blocked} refused`);
if (blocked > 0) console.warn("an armed order was refused by risk");
```

### `td.apiVersion` / `td.tradingDay` → `string` (read-only)

The CTP trader API version and current trading day.

```ts
console.log("CTP TD API", td.apiVersion, "day", td.tradingDay);
```

### `td.close()` / `td.droppedRecords`

As on [`CtpClient`](#ctpclient). The Trader uses an ordered lossless overflow
queue after its fixed ring fills; order/trade/query callbacks are not discarded.

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
value means the market-data consumer can't keep up. Market data drops oldest;
Trader callbacks spill losslessly and therefore keep this counter at zero.

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

- **`reqQry*`** (queries) → `Promise<unknown[]>` (array of rows; `[]` if empty),
  serialized through the shared query scheduler.
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
is generated from the CTP headers — 125 request methods.)

---

## Types

### `RiskLimits`

Pre-trade risk limits for [`setRiskLimits`](#tdsetrisklimitsconfig--this). All fields
are optional; **omit or `0` = disabled** for controls. `maxReferenceAgeMs` must
be positive and defaults to 3000 ms. `NaN`/`Infinity` throws.

| Field | Type | Meaning |
|---|---|---|
| `maxOrderVolume` | `number` | Max lots per single order. |
| `maxPriceDeviation` | `number` | Max `\|price − reference\| / reference` ratio (e.g. `0.02` = 2%); missing/stale reference fails closed. |
| `maxReferenceAgeMs` | `number` | Maximum quote age for deviation and monetary valuation; default 3000 ms. |
| `maxNotional` | `number` | Max notional (`max(order price, fresh reference) × volume × multiplier`) per order; missing/stale reference fails closed. |
| `maxOrdersPerSec` | `number` | Max opening-order sends per second (token bucket). |
| `orderBurst` | `number` | Token-bucket burst size. Default: `maxOrdersPerSec`. |
| `maxCloseOrdersPerSec` | `number` | Independent close-order send rate; disabled by default. |
| `closeOrderBurst` | `number` | Close-order bucket size. Default: `maxCloseOrdersPerSec`. |
| `maxAccountMargin` | `number` | Cap on total held + reserved account margin. Requires a fresh reference, multiplier and investor schedule; includes by-money/by-volume and combines relative schedules with the exchange base. Missing/stale inputs fail closed. |

```ts
const conservative: RiskLimits = { maxOrderVolume: 5, maxOrdersPerSec: 10, maxNotional: 2_000_000 };
td.setRiskLimits(conservative);
```

### `InstrumentPositionLimit`

`number | { long?: number; short?: number }` — the limit argument to
[`setInstrumentPositionLimit`](#tdsetinstrumentpositionlimitinstrumentid-limit--tdsetinstrumentpositionlimitslimits--this).
A number caps both sides; `{ long, short }` caps each (omit a side = unchanged,
`0` = clear; negative values are rejected).

```ts
const a: InstrumentPositionLimit = 100;                  // both sides ≤ 100
const b: InstrumentPositionLimit = { long: 50, short: 5 }; // long ≤ 50, short ≤ 5
```

### `SessionOptions`

Options for [`session`](#tdsessionopts--promise-identity-multipliers-marginrates-positions-orders-).
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
| Risk mode | `reduceOnly()` / `haltAll()` / `resume()` | Opens, or all order inserts; `haltAll()` also clears arms | `resume()` |
| Max order volume | `setRiskLimits({ maxOrderVolume })` | Per order | `0` / omitted |
| Max price deviation | `setRiskLimits({ maxPriceDeviation })` + reference | Per order | `0` / omitted (missing reference blocks) |
| Max notional | `setRiskLimits({ maxNotional })` | Per order | `0` / omitted |
| Open rate | `setRiskLimits({ maxOrdersPerSec, orderBurst })` | Per second | `0` / omitted |
| Close rate | `setRiskLimits({ maxCloseOrdersPerSec, closeOrderBurst })` | Per second | `0` / omitted |
| Max position lots | `setInstrumentPositionLimit(id, …)` | Per instrument, per side | `0` |
| Max position margin (per instrument) | `setInstrumentMarginLimit(id, …)` | Per instrument | `0` |
| Max position margin (account) | `setRiskLimits({ maxAccountMargin })` | Account | `0` / omitted |

Position-lot and position-margin caps count **committed = held (filled) +
in-flight (working order) volume**, so a burst of opens can't slip past a cap
before the fills report. Feed held positions via `syncPositions()` /
`seedPositions()` and working orders via `syncOrders()` (especially after a
reconnect). Query contract multipliers and both investor/exchange margin schedules before
enabling monetary caps; missing metadata blocks capped opens. All caps
are enforced in C++ on the send path; a breach
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
td.setRiskLimits({ maxOrderVolume: 5, maxNotional: 2_000_000, maxOrdersPerSec: 10, maxPriceDeviation: 0.02, maxAccountMargin: 5_000_000 });
td.setInstrumentPositionLimit(SYMBOL, { long: 10, short: 10 });
td.trackMarketData(md);              // fresh reference for deviation + monetary gates

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
  process.on(sig, () => { td.haltAll(); td.close(); md.close(); process.exit(0); });
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
  await td.syncOrders();        // rebuild reservations; marks positions uncertain
  await td.syncPositions();     // final held snapshot; preserve reservations
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

**Multi-leg offset flags are deliberately unsupported.** The local ledger is a
single-instrument/single-side ledger and cannot prove per-leg position or margin
for values such as `"00"`, `"30"`, or `"03"`. `reqOrderInsert()` and `arm()`
therefore require exactly one offset character and reject multi-leg templates
before they reach CTP. Use a higher-level gateway with a per-leg ledger if your
strategy needs exchange combination instruments.

Realtime callbacks and `syncOrders()` also detect unsupported working orders
placed elsewhere; while any remain, opening inserts fail
closed instead of guessing their per-leg exposure.

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

> `reqCombActionInsert` is also refused whenever local risk is configured or the
> mode is not `NORMAL`; it is outside this binding's quantitative-risk allowlist.
