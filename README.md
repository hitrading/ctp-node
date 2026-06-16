# ctp-node

High-performance, type-safe [CTP](http://www.sfit.com.cn/) (上期技术综合交易平台) binding for Node.js — for programmatic / quantitative futures trading.

- **Plain objects, idiomatic TypeScript.** Every CTP struct is a generated `interface` with camelCase fields (`tick.lastPrice`, `tick.instrumentId`); every CTP enum is a real TS `enum`. No hand-written marshalling.
- **Fast.** The CTP callback thread only memcpy's bytes into a lock-free ring; JS decodes straight from it into plain objects at **~8M ticks/sec** (166× headroom over a 50k/sec open-auction burst).
- **Hybrid API.** `EventEmitter` for streaming pushes (`rtn-depth-market-data`, `rtn-order`…) + `Promise` for request/response (login, queries, orders), correlated by request id with multi-row accumulation.
- **Pre-trade risk in C++.** Kill-switch, max order volume, max notional, price-deviation (fat-finger) guard, account-wide and per-instrument open-position cost caps, per-instrument max position (lots, per side), and a token-bucket rate limiter are enforced on the order path in native code — a JS GC pause can't defeat them.
- **Stable by construction.** No hand-marshalling, threadsafe-functions released properly, `Init()` deferred so `front-connected` is never missed, compiler-truth (`offsetof`) binary layout, gb18030 decoded in JS (no Windows code-page bug).

> Targets the regime CTP actually supports (snapshot-driven, ms-tolerant strategies: CTA, arbitrage, market making). True microsecond HFT is not achievable through CTP in any language.

## Requirements

Node ≥ 18. Prebuilt binaries are shipped for Windows/Linux/macOS (x64), so most users need no compiler. Other platforms build from source on install and need a C++ toolchain (Windows: VS Build Tools; Linux/macOS: clang/gcc). The CTP shared libraries are bundled next to the loaded addon and resolved automatically.

## Install

```sh
npm install ctp-node
```

## Quick start — market data

```ts
import { MarketData } from "ctp-node";

const md = new MarketData("./flow/md/", "tcp://180.168.146.187:10212");

md.on("front-connected", async () => {
  await md.login({ brokerId: "9999", userId: "xxxx", password: "xxxx" });
  md.subscribe(["rb2510", "ag2512"]);
});

md.on("rtn-depth-market-data", (tick) => {
  // tick is a plain object with camelCase fields
  console.log(tick.instrumentId, tick.lastPrice, tick.bidPrice1, tick.askPrice1);
});
```

## Quick start — trading

```ts
import { Trader } from "ctp-node";

const td = new Trader("./flow/td/", "tcp://180.168.146.187:10202");

// Pre-trade risk, enforced in C++ on every order:
td.riskSet({ maxOrderVolume: 10, maxOrdersPerSec: 20, maxPriceDeviation: 0.02, maxPositionCost: 5_000_000 });
td.trackMarketData(md); // feed live prices for the price-deviation guard
td.setMaxPositions({ rb2610: 100, au2610: 10, ru2610: { long: 100, short: 20 } }); // per-instrument lot caps
td.setMaxPositionCosts({ ag2608: 2_000_000, au2608: 5_000_000 });                   // per-instrument cost caps

td.on("front-connected", async () => {
  // One-call handshake (call again after a reconnect): authenticate → login →
  // confirm settlement → sync multipliers / positions / orders. Risk inputs are
  // fetched from CTP automatically — no manual multipliers/seeding.
  await td.session({ brokerId, userId, password, appId, authCode });

  // ...or hand-roll it if you need finer control:
  //   await td.reqAuthenticate({ brokerId, userId, appId, authCode });
  //   await td.reqUserLogin({ brokerId, userId, password });
  //   await td.reqSettlementInfoConfirm({ brokerId, investorId }); // real accounts
  //   await td.syncMultipliers(); await td.syncPositions(); await td.syncOrders();

  // Promise-based queries return all rows:
  const positions = await td.reqQryInvestorPosition({ brokerId, investorId });

  // Orders go through the C++ risk gate. reqOrderInsert resolves on submission
  // (CTP sends no ack for an accepted order); track the outcome via rtn-order /
  // rtn-trade. orderRef is auto-assigned a unique value if you leave it blank.
  await td.reqOrderInsert({
    brokerId, investorId, instrumentId: "rb2510",
    direction: "0",            // see Direction enum
    limitPrice: 3500, volumeTotalOriginal: 1,
  });
});

td.on("rtn-order", (order) => console.log("order update", order.orderStatus));
td.on("rtn-trade", (trade) => console.log("filled", trade.price, trade.volume));

// Emergency kill-switch (blocks all sends in C++ immediately):
// td.halt();  /  td.resume();
```

### Armed orders (latency-critical)

Fire an order from C++ the instant the market hits your trigger — evaluated on
the market-data callback thread, through the risk gate, with **no JS in the
loop** (no event-loop hop, no GC exposure):

```ts
const armed = trader.arm(md, {
  instrumentId: "rb2510",
  side: "buy",            // buy fires when ask ≤ trigger; sell when bid ≥ trigger
  triggerPrice: 3500,
  order: {
    brokerId, investorId, instrumentId: "rb2510", direction: "0",
    limitPrice: 3500, volumeTotalOriginal: 1, orderRef: "snipe-1",
  },
});
// One-shot. The ack/fill arrives via the normal rtn-order events (match orderRef).
// armed.disarm();
```

Enums are exported and typed:

```ts
import { Direction, OffsetFlag } from "ctp-node";
Direction.Buy;   // "0"
Direction.Sell;  // "1"
```

## API shape

- `new MarketData(flowPath, fronts)` / `new Trader(flowPath, fronts)` — `fronts` is a `tcp://` address or an array.
- Requests are camelCase methods taking a `Partial<...>` of the CTP field object and returning a `Promise`. `reqQry*` resolve with an array of rows; most requests resolve with the single response row.
- Exchange-bound inserts/actions (`reqOrderInsert`, `reqOrderAction`, …) resolve on **submission** — CTP returns no success response for an accepted order, only `rtn-order` / `rtn-trade` (correlate by `orderRef`). They reject only if the send is refused (risk gate, rate limit, or a CTP API error code).
- Streaming events use kebab-case names (`rtn-depth-market-data`, `rtn-order`, …); handlers get `(data, options)` where `options` carries `{ requestId?, isLast?, rspInfo? }`.
- `client.droppedRecords` reports records dropped under backpressure. Market
  data drops the **oldest** unread record (so you always see the freshest quote);
  the trader drops the **newest** instead, so a queued order/trade return is never
  silently discarded. Monitor it either way.
- `client.close()` releases the underlying CTP API. Call it **once**, at shutdown — a CTP client is create-once / reuse; reconnecting by recreating it deadlocks in the vendor DLL (see [Lifecycle](#lifecycle-create-once-reuse)).

## Lifecycle: create once, reuse

Create a `MarketData` / `Trader` **once** and keep it for the life of the
process. CTP reconnects on its own; handle a dropped link **in place** by
re-running your handshake when `front-connected` fires again (it fires on the
first connect *and* on every auto-reconnect) — never by destroying and
recreating the client.

```ts
const md = new MarketData("./flow/md/", front);
md.on("front-connected", async () => {     // first connect AND every reconnect
  await md.login({ brokerId, userId, password });
  md.subscribe(["rb2510"]);
});
// Trader: re-run the one-call handshake the same way
td.on("front-connected", () => td.session({ brokerId, userId, password, appId, authCode }));
```

> **Why.** `close()` calls the vendor `CThostFtdc*Api::Release()`, and the CTP
> DLLs **deadlock inside `Release()` after a few `Init()`/`Release()` cycles in
> one process** — a limitation of the CTP SDK itself, not of this binding. A
> "reconnect by recreate" loop will hang the whole process (and on Windows the
> wedged process ignores `SIGTERM`, so it must be force-killed with `taskkill /F`
> — until then it locks `build\Release\ctp.node` and breaks the next native
> build). Construct each client once and call `close()` only at shutdown. A
> one-time `console.warn` fires if an unusual number of clients are
> created/closed in a process (tune/disable with `CTP_RECREATE_WARN`). Details:
> [docs/native-hooks.md](docs/native-hooks.md#process-lifecycle-create-once-reuse).

## Architecture

```
CTP callback thread (C++)                 Node event loop (JS)
  OnRtn.../OnRsp... → memcpy bytes  ──►   coalesced doorbell → drain whole batch
  into lock-free SPSC ring,                → decode each record from the ring
  bump atomic index, ring doorbell           (monomorphic generated decoder)
  (on overflow: MD drops oldest,             → plain object → emit / resolve
   trader drops newest/reliable)
```

Everything below the public API is generated from the CTP headers
(`tradeapi/ThostFtdc*.h`) by `scripts/codegen/` — 466 struct interfaces, 318
enums, field layout tables (via `offsetof`), and the full trader SPI + request
dispatch. Run `npm run gen` after updating the headers.

## Build from source

```sh
npm run gen      # regenerate from CTP headers
npm run build    # gen + native (node-gyp) + tsc
npm test         # codec round-trip + md + trader (no network needed)
npm run bench    # decode throughput
```

## License

Apache-2.0
