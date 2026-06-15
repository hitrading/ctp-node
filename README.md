# ctp-node

High-performance, type-safe [CTP](http://www.sfit.com.cn/) (上期技术综合交易平台) binding for Node.js — for programmatic / quantitative futures trading.

- **Plain objects, idiomatic TypeScript.** Every CTP struct is a generated `interface` with camelCase fields (`tick.lastPrice`, `tick.instrumentId`); every CTP enum is a real TS `enum`. No hand-written marshalling.
- **Fast.** The CTP callback thread only memcpy's bytes into a lock-free ring; JS decodes straight from it into plain objects at **~8M ticks/sec** (166× headroom over a 50k/sec open-auction burst).
- **Hybrid API.** `EventEmitter` for streaming pushes (`rtn-depth-market-data`, `rtn-order`…) + `Promise` for request/response (login, queries, orders), correlated by request id with multi-row accumulation.
- **Pre-trade risk in C++.** Kill-switch, max order volume, max notional and a token-bucket rate limiter are enforced on the order path in native code — a JS GC pause can't defeat them.
- **Stable by construction.** No hand-marshalling, threadsafe-functions released properly, `Init()` deferred so `front-connected` is never missed, compiler-truth (`offsetof`) binary layout, gb18030 decoded in JS (no Windows code-page bug).

> Targets the regime CTP actually supports (snapshot-driven, ms-tolerant strategies: CTA, arbitrage, market making). True microsecond HFT is not achievable through CTP in any language.

## Requirements

Node ≥ 18, and a C++ toolchain for building from source (Windows: VS Build Tools; Linux/macOS: clang/gcc). Prebuilt binaries are shipped for common platforms, so most users need no compiler.

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
td.riskSet({ maxOrderVolume: 10, maxOrdersPerSec: 20 });

td.on("front-connected", async () => {
  await td.reqAuthenticate({ brokerId, userId, appId, authCode });
  await td.reqUserLogin({ brokerId, userId, password });

  // Promise-based queries return all rows:
  const positions = await td.reqQryInvestorPosition({ brokerId, investorId });

  // Orders go through the C++ risk gate:
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

Enums are exported and typed:

```ts
import { Direction, OffsetFlag } from "ctp-node";
Direction.Buy;   // "0"
Direction.Sell;  // "1"
```

## API shape

- `new MarketData(flowPath, fronts)` / `new Trader(flowPath, fronts)` — `fronts` is a `tcp://` address or an array.
- Requests are camelCase methods taking a `Partial<...>` of the CTP field object and returning a `Promise`. `reqQry*` resolve with an array of rows.
- Streaming events use kebab-case names (`rtn-depth-market-data`, `rtn-order`, …); handlers get `(data, options)` where `options` carries `{ requestId?, isLast?, rspInfo? }`.
- `client.droppedRecords` reports records dropped under backpressure.
- `client.close()` releases the underlying CTP API.

## Architecture

```
CTP callback thread (C++)                 Node event loop (JS)
  OnRtn.../OnRsp... → memcpy bytes  ──►   coalesced doorbell → drain whole batch
  into lock-free SPSC ring,                → decode each record from the ring
  bump atomic index, ring doorbell           (monomorphic generated decoder)
  (drop-newest if full)                      → plain object → emit / resolve
```

Everything below the public API is generated from the CTP headers
(`tradeapi/ThostFtdc*.h`) by `scripts/codegen/` — 466 struct interfaces, 316
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
