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
- [Types](#types) — `RiskLimits`, `SessionOptions`, `CallbackOptions`, `RspInfo`, `MdLoginReq`
- [Enums & struct types](#enums--struct-types)
- [Risk controls at a glance](#risk-controls-at-a-glance)
- [Recipes](#recipes) — strategy skeleton · reconnect handling · combination orders

Upgrading from 4.x? See [MIGRATION.md](MIGRATION.md) for the removed ledger,
caps and armed orders (`arm()`, local market-data-triggered orders). CTP's own
parked-order requests (`reqParkedOrderInsert` etc.) remain and are handled as
unsupported requests (see [Risk configuration](#risk-configuration)).

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
install (a C++ toolchain + Python are required; on macOS, the Xcode Command Line
Tools). The bundled CTP macOS frameworks are the universal 6.7.13 build, the
same CTP version as the Windows/Linux libraries.

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

td.setRiskLimits({ maxOrderVolume: 10, maxOrdersPerSec: 20 }); // per-order gates in C++
// Account-level caps (margin, position lots) live in your own trading layer.

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

**Pre-trade risk runs in C++, and it is stateless.** The risk limits
(`setRiskLimits`) and modes (`reduceOnly` / `haltAll`) are enforced on the
order-send path inside the native addon, before the order reaches CTP — no JS
round trip. A blocked order makes `reqOrderInsert()` reject; the reason is in
the error message. Every rule judges one order from the order itself, the latest
market-data reference (`trackMarketData`) and static instrument metadata (from
`reqQryInstrument`). The binding keeps no position or working-order ledger, so
there is nothing to rebuild after a reconnect and nothing that can disagree with
your own books. Account-level exposure caps — account margin, per-instrument
margin, position lots — belong to your trading layer, which owns the account
facts: see [Account facts and account-level caps](#account-facts-and-account-level-caps).

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
> the native cache remains fail-closed: the first late final
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

The trading (交易) client: order entry, queries, and the stateless native
pre-trade risk gate. Extends [`CtpClient`](#ctpclient); every
generated `reqXxx` request method is available (see
[request methods](#generated-request-methods)).

### `new Trader(flowPath, fronts)`

Same parameters as [`MarketData`](#new-marketdataflowpath-fronts). The trader
remembers the confirmed account, so the `sync*` helpers never take account
fields, and seeds its auto-`OrderRef` counter past the broker's `maxOrderRef` so
refs never collide with a prior session.

```ts
const td = new Trader("./flow/td/", "tcp://182.254.243.31:30002");
// failover fronts:
const td2 = new Trader("./flow/td/", ["tcp://182.254.243.31:30002", "tcp://182.254.243.31:30001"]);
```

### `td.session(opts)` → `Promise<{ identity, multipliers, marginRates }>`

One-call post-connect handshake: **authenticate → login → confirm settlement →
sync multipliers / optional margin rates**. Call it from the `front-connected`
handler (and again after a reconnect). Returns the request-correlated
`identity: { frontId, sessionId, maxOrderRef }` plus row counts from the sync.
Here `maxOrderRef` is the value reported by that login response; the Trader's
automatic allocator may retain a higher process-local high-water mark.

`session()` is also what binds the native order gate to the trading account. At
handshake start the previous session's order authority is revoked; ordinary
order inserts are accepted again only after this handshake has confirmed the new
login's `frontId` / `sessionId` and the trading account (`investorId`). The
login itself is only the protocol step: the account is confirmed — natively and
in JS together — after the settlement confirmation has succeeded (or right after
login with `confirmSettlement: false`), and only if the connection has not
changed meanwhile. While the confirmation is pending no account-bound send is
accepted; if a later step (multiplier or margin-rate sync) fails, the
confirmation is revoked again. A newly
created Trader starts unconfirmed, and every front connect/disconnect and every
login/logout request revokes the confirmation, so run `session()` on every
`front-connected`. It does not query positions or working orders: keep those in
your own books (see [Account facts and account-level caps](#account-facts-and-account-level-caps)).

`opts` is a [`SessionOptions`](#sessionoptions):

The login response must contain a concrete `frontId` / `sessionId` and the same
`brokerId` / `userId` that were requested. An empty or mismatched success
payload is rejected because those fields identify the session the native order
gate is bound to; accepting an old/zero session or another account would let
orders go out under a stale identity.

| Field | Type | Meaning |
|---|---|---|
| `brokerId` | `string` | Broker id (required). |
| `userId` | `string` | Investor id (required). |
| `investorId` | `string?` | Trading account confirmed for orders. Defaults to `userId`; set it when the login operator differs from the investor. |
| `password` | `string` | Password (required). |
| `appId` | `string?` | Terminal app id for authentication. SimNow: `"simnow_client_test"`. |
| `authCode` | `string?` | Terminal auth code. SimNow: 16 zeros. Omit both `appId`/`authCode` to skip authentication. |
| `confirmSettlement` | `boolean?` | Confirm the settlement statement after login (real accounts must, or orders are rejected). Every echoed row that carries a broker or investor must name this account, otherwise the handshake fails; a legal empty final packet is accepted. Default `true`. |
| `sync` | `object?` | `{ multipliers?: boolean \| string[], marginRates?: { instrumentIds: string[], hedgeFlag: "1" \| "2" \| "3" \| "5" \| "6" \| "7" } }`. Multipliers default on (all instruments; pass a list to scope it, `false` to skip); margin rates require an explicit bounded contract list and hedge flag because CTP needs one query per contract and keeps separate schedules per hedge flag. |

```ts
// SimNow full handshake
td.on("front-connected", async () => {
  const counts = await td.session({
    brokerId: "9999", userId: "id", password: "pw",
    appId: "simnow_client_test", authCode: "0000000000000000",
  });
  console.log("synced", counts); // { identity, multipliers, marginRates }
});

// scope the multiplier sync to the symbols you trade (faster cold start)
await td.session({
  brokerId: "9999", userId: "id", password: "pw",
  sync: {
    multipliers: ["rb2510", "au2508"],
    marginRates: { instrumentIds: ["rb2510", "au2508"], hedgeFlag: "2" },
  },
});

// skip the settlement confirm (environments that don't need it)
await td.session({ brokerId: "9999", userId: "id", password: "pw", confirmSettlement: false });
```

> Each step wraps the matching request/sync method: `reqAuthenticate(...)` →
> `reqUserLogin(...)` → `reqSettlementInfoConfirm(...)` → `syncMultipliers()` →
> `syncMarginRates([...])`. A hand-rolled flow can run raw queries and
> `syncMultipliers()`, but a bare `reqUserLogin()` publishes no order account:
> only `session()` confirms the trading account, natively and in JS.
> `reqOrderInsert()` and `syncMarginRates()` after a hand-rolled login are
> refused (`a confirmed Trader login is required` /
> `syncMarginRates requires a successful trader login first`), and the native
> gate refuses other account-bound sends (`session account not confirmed`).

### `td.reqOrderInsert(req?)` → `Promise<void>`

Insert an order through the C++ pre-trade risk gate. **Resolves on submission** —
CTP sends no success acknowledgement for an accepted order, only `rtn-order` /
`rtn-trade` (correlate by `orderRef`). It **rejects** only if the send is refused
(risk gate, rate limit, or a CTP API error code). A blank `orderRef` is assigned
a unique numeric value automatically.

A confirmed `session()` is required. `brokerId` and `investorId` are filled from
that session; supplying a different value is rejected before encoding, so an
order can only go out under the account the native gate has confirmed.

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

> **Only single-instrument GFD, FAK and FOK limit orders are accepted.** Use `timeCondition: "3", volumeCondition: "1"` for GFD; `"1", "1"` for FAK; and `"1", "3"` for FOK. `reqOrderInsert` deliberately rejects (with a `TypeError`) anything the native gate cannot judge as one instrument at one limit price: other time/volume-condition combinations, min-volume/conditional/stop/swap/force-close variants, and multi-leg offset/hedge flags.

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
methods resolve one row. CTP rate-limits queries per session.
Every generated query shares a per-Trader scheduler spaced by the limits the
logged-in front grants (see `td.frontInfo`): never more than `qryFreq`, half of
`ftdPkgFreq`, or 10 per second, and at least 1100 ms apart until those limits are
known. This includes concurrent direct calls; it keeps at most one
query in flight, so callers do not add `setTimeout(1000)`. Request data is
snapshotted when the method is called, and disconnect/login/logout transitions
reject queued work from the old context. The scheduler is per client (not
process-global) and accepts at most 256 active/queued queries. Raw queries are
paced but not retried. The
[`sync*`](#tdsyncmultipliersinstrumentids--promisenumber) helpers additionally
retry transient `-2`/`-3`/90/154 "not ready" failures and required empty cold-start
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
(合约乘数) and product classes — it sources them **directly** from CTP's
`OnRspQryInstrument` response, so this calls no JS setter. Needed for the
multiplier-accurate `maxNotional` check. No argument queries all instruments in
one request; pass a symbol list to scope it. An explicit empty list is a no-op
returning `0`; only an omitted argument means "query all". Retries through
cold-start flow control. Returns the count of contracts whose final callback row
has a usable multiplier.

Starting an instrument refresh invalidates the old multiplier of the queried
contract (of every contract when the query has no filter at all) before the
request is sent, so a failed/empty refresh cannot leave stale metadata active;
an enabled `maxNotional` check fails closed for those contracts until a refresh
succeeds. A query filtered only by exchange, exchange instrument code or product
(6.0.1+) invalidates nothing and merges on success, so it never wipes contracts
outside its scope. The rows of an instrument query are staged natively under its
request id and take effect together only when that query's final packet arrives
without an error. An error final packet, the generic `rsp-error` of that
request, its idle timeout, a front connect/disconnect, a deliberate login/logout
or a newer instrument query discards them, and late rows of a superseded query
are ignored. A row whose InstrumentID cannot be read clears every contract on
success, even for a filtered query.

```ts
const n = await td.syncMultipliers();          // all instruments
await td.syncMultipliers(["rb2510", "au2508"]); // just these
console.log(n, "contracts with a multiplier");
```

### `td.syncMarginRates(instrumentIds, hedgeFlag?)` → `Promise<number>`

Resolve investor long/short by-money/by-volume margin schedules for a bounded
contract universe (default hedge flag `"1"`, speculation), for **your own**
margin calculations — the native gate does not use them. Investor queries are
grouped by the ProductID in a fresh complete CTP catalog; a contract-level row
takes precedence over the product row even when it is invalid. A second
exchange-base query is issued only for a relative (`IsRelative`) row, and the
relative investor schedule is added to that base. The shared scheduler spaces
every query. Returns the number of contracts with a usable schedule.

Every row is checked for ownership before use (6.0+): it must carry your
BrokerID and no InvestUnitID, and an ExchangeID, when present, must be the
contract's exchange in the instrument catalog (6.0.1+; both investor and
exchange-base rows); a single-investor row must name your InvestorID, an
investor-group row your InvestorGroupID (queried once with `reqQryInvestor`
only when an otherwise attributable investor-group row appears), and an
all-investors row is accepted whatever wildcard InvestorID it carries. The
instrument catalog must give every requested product exactly one non-empty
exchange (CTP catalogs do); otherwise the sync fails. When several ranges cover a contract the most
specific wins (single > group > all); disagreeing rows of one range, an
exchange-base row of another broker or an unavailable investor group fail the
sync rather than silently taking the last row.

Schedules previously resolved for the requested contracts (and for the other
contracts of the same products) are dropped during the refresh. The new
results are kept only when every requested contract resolved; a partial refresh
keeps none of them.

### `td.readMarginRates(instrumentIds, hedgeFlag)` → `CtpResolvedMarginRate[]`

Copies of the schedules resolved by the last complete `syncMarginRates()` (or
`session({ sync: { marginRates } })`) in the current session:
`{ instrumentId, hedgeFlag, longMarginRatioByMoney, longMarginRatioByVolume,
shortMarginRatioByMoney, shortMarginRatioByVolume }`. It throws when a requested
contract has no completed schedule, and after a reconnect or login/logout until
the schedules are synced again; old values are never returned.

```ts
await td.syncMarginRates(["rb2510"], "1");
const [rb] = td.readMarginRates(["rb2510"], "1");
// one long lot at 3500 on a x10 contract, by your own formula:
const margin = 3500 * 10 * rb.longMarginRatioByMoney + rb.longMarginRatioByVolume;
```

### Risk configuration

#### `td.setRiskLimits(config)` → `this`

Publish pre-trade risk limits to the C++ enforcer (takes effect at once). A
non-finite (`NaN`/`Infinity`) limit is **rejected with a throw** rather than
silently disabling the control. `config` is a [`RiskLimits`](#risklimits); omit a
field or pass `0` to disable that control; negative values are rejected. The object replaces the
whole prior configuration; `maxReferenceAgeMs` is the exception: it must be
positive and defaults to 3000 ms when omitted. Unknown fields (including removed
names such as `maxMargin` and `maxAccountMargin`) are rejected instead of being
ignored.

The volume, deviation and notional limits and the opening rate apply to
**opening** orders. A closing order still needs the confirmed session and
well-formed fields, but among the risk rules it is refused only in `HALT_ALL`
(and by the close-order rate limiter when configured), so exits keep working
when a reference quote or metadata is missing.

`maxNotional` is enabled only for CTP Futures/TAS product classes: option premium
and combination prices are not treated as futures notional, so an unknown or
unsupported product class fails closed while the guard is on. Notional is
valued at the greater of the order price and a fresh market reference, so a low
sell/open limit cannot understate it. Attach market data with
[`trackMarketData()`](#tdtrackmarketdatamd--this); a missing quote, or one older
than `maxReferenceAgeMs`, makes the deviation and notional checks fail closed for
opening orders.

```ts
// full set
td.setRiskLimits({
  maxOrderVolume: 10,          // ≤ 10 lots per opening order
  maxPriceDeviation: 0.02,     // ≤ 2% from the reference price
  maxNotional: 5_000_000,      // ≤ 5M Futures/TAS notional per opening order
  maxOrdersPerSec: 20,         // token-bucket rate limit
  orderBurst: 40,              // bucket size (default: maxOrdersPerSec)
  maxCloseOrdersPerSec: 10,    // optional independent close-order rate
  closeOrderBurst: 20,
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
blocks close inserts until `resume()`. Mode transitions share the final
vendor-API send barrier with every order insert, so no forbidden order can be
sent after the call returns. `resume()` restores `NORMAL`.

Advanced mutations (complex inserts, order modify, offset setting, spread/hedge
confirmation) cannot be judged by the per-order rules, so they are forwarded only
in `NORMAL`, for a confirmed session, and when no local risk control (limit or
rate limiter) is configured; otherwise they fail with `-10006`. The generator
requires every future non-query SDK request to be classified; an unreviewed
method fails code generation instead of defaulting to unguarded.

Each such request is judged on its own and leaves no state behind. The native
gate does not track the exposure that advanced requests — or orders from another
terminal — create; accounting for it is your trading layer's job.

Bank/futures transfers are subject to the same rule. They change available
collateral, so they are forwarded only in `NORMAL` mode, for a confirmed
session, when every local risk control is disabled. Authentication and transfer
authorization remain the caller's responsibility.

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

#### `td.trackMarketData(md)` → `this`

Provide the live reference price used by the price-deviation and notional
checks. Wires the `MarketData` feed's C++ snapshot cache straight into this
Trader's risk engine, so the reference is read **in C++ on the order-send path**
— no JS round-trip. Without a reference, or when its age exceeds
`maxReferenceAgeMs` (default 3000 ms), every enabled deviation or notional check
fails closed for opening orders.

```ts
td.setRiskLimits({ maxPriceDeviation: 0.02 });

// live reference from the feed (read in C++ on every order)
td.trackMarketData(md);
```

> Contract multipliers and product classes (for `maxNotional`) are **not** fed
> from JS: the C++ risk engine sources them directly from CTP's
> `OnRspQryInstrument` callback; a `reqQryInstrument` refreshes them when its
> final packet arrives without an error
> (see [`syncMultipliers`](#tdsyncmultipliersinstrumentids--promisenumber)).
> Margin schedules are not used by the native gate at all;
> [`syncMarginRates()`](#tdsyncmarginratesinstrumentids-hedgeflag--promisenumber)
> and `readMarginRates()` resolve them for your own calculations. Missing
> metadata is never guessed.

#### Account facts and account-level caps

The native gate keeps no position, working-order or funds ledger, and it has no
account-level caps. CTP's position snapshot and its realtime order/trade stream
share no common sequence number, so a ledger inside the binding could only
duplicate your own books — and could disagree with them. Keep the account facts
in your trading layer and judge account-wide limits (account margin,
per-instrument margin, position lots) there, before calling `reqOrderInsert()`:

- **Snapshot** after every `session()` (first connect and every reconnect):
  `reqQryInvestorPosition`, `reqQryOrder` and `reqQryTradingAccount`.
- **Stay current** from `rtn-order` / `rtn-trade`. CTP also delivers the
  returns of the investor's other sessions, so orders placed from another
  terminal on the same account show up there too. Reconciling a snapshot with
  the realtime stream is your book's job.
- **Margin estimates** for orders you are about to send: multipliers from
  `reqQryInstrument`, schedules from `syncMarginRates()` / `readMarginRates()`.

```ts
const acct = { brokerId: "9999", investorId: "id" };
let ready = false;
td.on("front-disconnected", () => { ready = false; }); // stop new exposure at once
td.on("front-connected", async () => {
  await td.session({ brokerId: acct.brokerId, userId: acct.investorId, password: "pw",
    sync: { marginRates: { instrumentIds: ["rb2510"], hedgeFlag: "1" } } });
  // Exactly one funds row of your broker and currency. It carries no InvestorID,
  // and its AccountID may differ from the InvestorID: keep the one returned.
  const funds = (await td.reqQryTradingAccount({ ...acct, currencyId: "CNY" }))
    .filter((f) => f.brokerId === acct.brokerId && f.currencyId === "CNY");
  if (funds.length !== 1) throw new Error("expected exactly one CNY funds row");
  book.reset(await td.reqQryInvestorPosition(acct), await td.reqQryOrder(acct), funds[0]);
  ready = true;
});
td.on("rtn-order", (o) => book.onOrder(o));
td.on("rtn-trade", (t) => book.onTrade(t));

// your own account-level check, then the native per-order gate
const [rb] = td.readMarginRates(["rb2510"], "1");
const add = 3500 * 1 * 10 * rb.longMarginRatioByMoney + 1 * rb.longMarginRatioByVolume;
if (ready && book.marginInUse() + add <= MY_ACCOUNT_MARGIN_CAP && book.longLots("rb2510") + 1 <= MY_LOT_CAP) {
  await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 1 });
}
```

`book` stands for your own ledger; the binding does not provide one.

### `td.apiVersion` / `td.tradingDay` → `string` (read-only)

The CTP trader API version and current trading day.

```ts
console.log("CTP TD API", td.apiVersion, "day", td.tradingDay);
```

### `td.frontInfo` → `FrontInfo` (read-only)

The connected front and the per-second limits it grants this session: `qryFreq`
queries and `ftdPkgFreq` FTD packets (login, query, order and cancel alike; the
front silently holds the excess until the next second). Valid after login; query
spacing follows it automatically and returns to 1100 ms on any front change or
login/logout transition.

```ts
const { frontAddr, qryFreq, ftdPkgFreq } = td.frontInfo;
console.log(frontAddr, "queries/s", qryFreq, "packets/s", ftdPkgFreq);
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
- **All other requests** → `Promise<...>` resolving with the first row of their
  own `OnRsp*` response; `reqSettlementInfoConfirm` resolves every echoed row
  (`Promise<unknown[]>`) so the caller can check the account on each.

A request is settled only by its own response callback (`ReqXxx` → `OnRspXxx`;
every login variant → `OnRspUserLogin`) or by the generic `rsp-error`. Another
callback type that carries the same request id is still emitted as an event but
neither resolves, rejects nor feeds the request; if nothing else arrives, the
request times out and the error names the ignored callback.

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
be positive and defaults to 3000 ms. `NaN`/`Infinity` throws. Every rule judges
one order; there is no account-level field.

| Field | Type | Meaning |
|---|---|---|
| `maxOrderVolume` | `number` | Max lots per single opening order. |
| `maxPriceDeviation` | `number` | Max `\|price − reference\| / reference` ratio for an opening order (e.g. `0.02` = 2%); missing/stale reference fails closed. |
| `maxReferenceAgeMs` | `number` | Maximum quote age for the deviation and notional checks; default 3000 ms. |
| `maxNotional` | `number` | Max Futures/TAS notional (`max(order price, fresh reference) × volume × multiplier`) per opening order; missing/stale reference, missing multiplier or another product class fails closed. |
| `maxOrdersPerSec` | `number` | Max opening-order sends per second (token bucket). |
| `orderBurst` | `number` | Token-bucket burst size. Default: `maxOrdersPerSec`. |
| `maxCloseOrdersPerSec` | `number` | Independent close-order send rate; disabled by default. |
| `closeOrderBurst` | `number` | Close-order bucket size. Default: `maxCloseOrdersPerSec`. |

```ts
const conservative: RiskLimits = { maxOrderVolume: 5, maxOrdersPerSec: 10, maxNotional: 2_000_000 };
td.setRiskLimits(conservative);
```

### `SessionOptions`

Options for [`session`](#tdsessionopts--promise-identity-multipliers-marginrates-).
See the table there.

```ts
const opts: SessionOptions = {
  brokerId: "9999", userId: "id", password: "pw",
  appId: "simnow_client_test", authCode: "0000000000000000",
  confirmSettlement: true,
  sync: { multipliers: ["rb2510"], marginRates: { instrumentIds: ["rb2510"], hedgeFlag: "1" } },
};
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
| Risk mode | `reduceOnly()` / `haltAll()` / `resume()` | Opens, or all order inserts (cancels always pass) | `resume()` |
| Max order volume | `setRiskLimits({ maxOrderVolume })` | Per opening order | `0` / omitted |
| Max price deviation | `setRiskLimits({ maxPriceDeviation })` + reference | Per opening order | `0` / omitted (missing reference blocks) |
| Max notional | `setRiskLimits({ maxNotional })` + reference + multiplier | Per opening order | `0` / omitted |
| Open rate | `setRiskLimits({ maxOrdersPerSec, orderBurst })` | Per second | `0` / omitted |
| Close rate | `setRiskLimits({ maxCloseOrdersPerSec, closeOrderBurst })` | Per second | `0` / omitted |
| Account margin, per-instrument margin, position lots | your own trading layer | Account / instrument | — |

The native controls are stateless: each judges the order at hand from the order
itself, the latest reference quote (`trackMarketData`) and static contract
metadata (`reqQryInstrument`), so none of them needs a position or order
snapshot and none of them changes after a reconnect. All of them are enforced
in C++ on the send path; a breach makes `reqOrderInsert()` reject with the
reason in the message. Limits that depend on what the account holds or has
working are judged in your own trading layer — see
[Account facts and account-level caps](#account-facts-and-account-level-caps).

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

// 1) Configure risk BEFORE any order can be sent (per-order gates, enforced in C++).
td.setRiskLimits({ maxOrderVolume: 5, maxNotional: 2_000_000, maxOrdersPerSec: 10, maxPriceDeviation: 0.02 });
td.trackMarketData(md);              // fresh reference for the deviation + notional gates
const MAX_NET_LOTS = 10;             // account-level cap: judged here, in your own layer

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

// 4) Position is derived from FILLS, never assumed from sends. (A real book also
//    starts from reqQryInvestorPosition after each session(); see the reconnect recipe.)
let position = 0; // net lots (+long / -short)
td.on("rtn-trade", (t: Trade) => {
  position += t.direction === Direction.Buy ? t.volume : -t.volume;
  console.log("fill", t.price, "x", t.volume, "-> net position", position);
});

// 5) The strategy: turn each tick into a target and trade the difference.
md.on("rtn-depth-market-data", async (tick: DepthMarketData) => {
  if (tick.instrumentId !== SYMBOL) return;
  // your logic: target net lots, clamped to your own position cap
  const want = Math.max(-MAX_NET_LOTS, Math.min(MAX_NET_LOTS, signal(tick)));
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
const acct = { brokerId: CREDS.brokerId, investorId: CREDS.userId };
td.on("front-connected", async () => {
  // session() re-authenticates, re-logs-in, re-confirms settlement, re-syncs
  // multipliers and re-confirms the account to the native order gate (orders are
  // refused until it does). The gate itself has nothing to rebuild.
  await td.session({ ...CREDS });
  // Your books, however, may have missed order/trade returns while the link was
  // down: rebuild them from fresh snapshots, then keep applying rtn-order /
  // rtn-trade.
  book.reset(await td.reqQryInvestorPosition(acct), await td.reqQryOrder(acct));
});

td.on("front-disconnected", (reason) => {
  console.warn("trader link down (code", reason, ") — CTP will auto-reconnect");
  // pending request Promises have already rejected; pause new signals until
  // the next front-connected if your strategy needs a confirmed session.
});
```

CTP gives no common sequence number between a position/order snapshot and the
realtime return stream, so reconciling the two belongs to your book (the binding
deliberately keeps no copy of its own). Do not replace `session()` with a raw
`reqUserLogin()` here: only `session()` confirms the account for order inserts.

### Combination & multi-leg orders

**Close-today vs close-yesterday.** SHFE/INE distinguish them, so `"close"` isn't
enough — pick `CloseToday` (`"3"`) or `CloseYesterday` (`"4"`). To flatten 5 long
lots that are 2 opened today + 3 from prior days, send two orders:

```ts
import { Direction, OffsetFlag } from "@hitrading/ctp-node";

await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseToday,     limitPrice: px, volumeTotalOriginal: 2 });
await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseYesterday, limitPrice: px, volumeTotalOriginal: 3 });
```

**Multi-leg offset flags are deliberately unsupported.** The native gate judges
one instrument at one limit price and cannot judge per-leg prices or volumes for
values such as `"00"`, `"30"`, or `"03"`. `reqOrderInsert()` therefore requires
exactly one offset character and rejects multi-leg templates before they reach
CTP. Use a higher-level gateway with per-leg accounting if your strategy needs
exchange combination instruments.

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
