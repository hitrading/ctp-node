<!-- LANG-SWITCH -->
**English** · [简体中文](troubleshooting.zh-CN.md)

# Troubleshooting & FAQ

Common failures and how to fix them. See also the [API reference](API.md) and the
[native internals](native-hooks.md).

- [Connection](#connection) · [Login / authentication](#login--authentication) · [Queries & rate limits](#queries--rate-limits)
- [Order rejections](#order-rejections) · [Limit-up / limit-down](#limit-up--limit-down) · [Performance & memory](#performance--memory)
- [Lifecycle & reconnect](#lifecycle--reconnect) · [Build & install](#build--install) · [Error-code reference](#error-code-reference)

---

## Connection

### `front-connected` never fires / no ticks arrive

The front address is wrong or unreachable. SimNow **rotates its fronts** — the
old `180.168.146.187:10201/10211` are dead; as of writing the live site is
**上期技术-电信二**: trade `tcp://182.254.243.31:30002`, md `tcp://182.254.243.31:30012`.

1. Find your current fronts in the SimNow client under **代理及服务器配置** (the
   交易服务器地址 / 行情服务器地址 it shows, with a live ping).
2. Pass them explicitly: `new Trader("./flow/td/", "tcp://HOST:PORT")` /
   `new MarketData("./flow/md/", "tcp://HOST:PORT")`.
3. Confirm raw TCP reachability first:
   ```js
   require("net").connect({ host: "182.254.243.31", port: 30002, timeout: 5000 })
     .on("connect", () => console.log("TCP OPEN")).on("error", (e) => console.log(e.code));
   ```
   `timeout`/`ECONNREFUSED` = wrong front or blocked egress (VPN/firewall), not a
   binding problem.

### Connects but no market data

Connected ≠ subscribed. You must `await md.login(...)` then `md.subscribe([...])`
inside the `front-connected` handler. Also: a contract only ticks when it trades —
outside its session (e.g. night-session-only products during the day) it is
silent even when subscribed.

---

## Login / authentication

| Symptom | Cause | Fix |
|---|---|---|
| login rejects, "CTP:不合法的登录" | wrong brokerId / userId / password | SimNow brokerId is `9999`; check the account. |
| `reqAuthenticate` fails | wrong appId / authCode (terminal authentication) | SimNow: `appId: "simnow_client_test"`, `authCode: "0000000000000000"`. Real accounts use the broker-issued values. |
| "CTP:还没有初始化" | called a request before `front-connected` | do login/queries inside the `front-connected` handler. |
| orders rejected right after login on a real account | settlement statement not confirmed | call `reqSettlementInfoConfirm(...)` (or just use `session()`, which does it). |
| first query after login is empty / rejected | queried too soon (cold start) | use the `sync*` helpers (they retry through flow control), or wait ~1s. |

`session()` does authenticate → login → confirm → sync in the right order with
the right retries — prefer it over hand-rolling unless you need fine control.

```ts
td.on("front-connected", () => td.session({
  brokerId: "9999", userId: "id", password: "pw",
  appId: "simnow_client_test", authCode: "0000000000000000",
}).catch((e) => console.error("session failed", e.errorId, e.errorMsg)));
```

---

## Queries & rate limits

CTP throttles **queries to ~1 per second** per session. Issuing them too fast (or
too soon after login) returns empty or rejects with a flow-control error.

- Use `syncMultipliers()` / `syncPositions()` / `syncOrders()` — they retry with
  back-off built in.
- For raw `reqQry*`, space calls ≥ ~1.1 s apart.
- Send-side flow control also applies to **orders**: CTP caps the send rate.
  Configure `riskSet({ maxOrdersPerSec, orderBurst })` so the C++ token bucket
  paces you under the limit instead of getting exchange rejections.

The send-return codes for this: `-2` = too many unhandled requests in flight,
`-3` = per-second send rate exceeded (see [reference](#error-code-reference)).

---

## Order rejections

`reqOrderInsert()` **resolves on submission** and only rejects if the *send* is
refused; an order rejected by the **exchange** comes back as an
`err-rtn-order-insert` event (with `options.rspInfo.errorMsg`), and status
changes arrive as `rtn-order`. Watch both.

| `errorMsg` (CTP, Chinese) | Meaning | Fix |
|---|---|---|
| `不允许重复报单` | duplicate / non-numeric `OrderRef` | **Don't pass your own `orderRef`** — leave it blank and the Trader auto-assigns a unique numeric ref (seeded past the broker's `maxOrderRef`). If you must set it, use a strictly increasing number. |
| `资金不足` | insufficient margin | reduce size, or close other positions; check `reqQryTradingAccount().available`. |
| `平仓量超过持仓量` / `可平仓位不足` | closing more than you hold | check the position; on SHFE/INE pick the right offset (close-today vs close-yesterday). |
| `平今/平昨` errors | wrong offset flag on SHFE/INE | use `OffsetFlag.CloseToday` (`"3"`) vs `OffsetFlag.CloseYesterday` (`"4"`), not plain `Close`. |
| `报单价格超出涨跌停板` | price beyond the daily limit | clamp the price to within ±limit; for a resting (non-filling) order use a few ticks off the touch, not far. |
| `报单字段有误` | malformed `InputOrder` | include the full field set CTP needs (`combHedgeFlag`, `orderPriceType`, `timeCondition`, `volumeCondition`, `contingentCondition`, `forceCloseReason`, …). See the [order recipe](API.md#recipes). |
| `CTP:撤单找不到对应报单` | cancelling an order that's already gone | the order already filled/cancelled; ignore. |

If your own pre-trade risk blocked it, the **rejected Promise** message says so
(`blocked by pre-trade risk: ...`, `rate limited`, `position ... limit`) — that's
your `riskSet`/`setMaxPosition`/`halt`, not the exchange.

---

## Limit-up / limit-down

At **limit-down** there are no bids, and at **limit-up** no asks. CTP fills the
empty side with the sentinel `DBL_MAX` (`Number.MAX_VALUE`, ~1.8e308), **not** 0.

- **Armed triggers** already guard this: a sell arms on `bid ≥ trigger`, and the
  binding ignores a `DBL_MAX` bid so it can't fire a panic sell at limit-down
  (likewise buys vs a `DBL_MAX` ask).
- **Your own logic must guard it too** — before using `tick.bidPrice1` /
  `lastPrice`, check `px > 0 && px < 1e300`. The risk engine's reference-price,
  cost, and notional paths already reject the sentinel.
- **Cancelling at limit-down works** — `reqOrderAction` isn't price-gated, and
  the kill-switch (`halt()`) deliberately leaves cancels open so you can always
  pull resting orders even while halted.

```ts
md.on("rtn-depth-market-data", (t) => {
  if (!(t.bidPrice1 > 0) || t.bidPrice1 >= 1e300) return; // no bid (limit-down)
  // ...safe to use the quote
});
```

---

## Performance & memory

Subscribing the **entire market** is fine. Measured live (SimNow, 18,111
instruments, per-tick SMA on every one + timer-driven order/cancel): RSS
plateaued ~124 MB, CPU 0–2%, **0 dropped records**, ~3 µs/tick strategy cost,
event-loop lag normal. The decoder sustains ~8 M ticks/s, so a full-market feed
has large headroom.

- **`droppedRecords > 0` and climbing** = your JS handlers can't keep up and the
  market-data ring is shedding the oldest ticks ("行情挤压"). Move heavy work off
  the tick path (batch it, or offload to a worker); the trader ring is reliable
  (drop-newest) so order/trade returns are never dropped.
- **Memory grows unbounded** = you're retaining objects (e.g. pushing every tick
  into an array forever). The binding itself decodes into transient objects and a
  fixed-size ring; it does not leak. Keep only bounded per-instrument state.
- **The big `reqQryInstrument({})`** (all ~18k instruments) streams over ~30–50 s
  and resolves as one array — expected; budget for that cold-start time.

---

## Lifecycle & reconnect

**Never reconnect by destroying and recreating a client.** The CTP vendor DLL
deadlocks inside `Release()` after a few `Init()`/`Release()` cycles per process.
CTP reconnects on its own — re-run your handshake **in place** on every
`front-connected`. Call `close()` once, at shutdown. See
[native-hooks.md → Process lifecycle](native-hooks.md#process-lifecycle-create-once-reuse).

- A one-time `console.warn` (`CTP_RECREATE_WARN`) fires if you create/close too
  many clients in one process — heed it.
- **Windows: the process hangs on exit / can't be killed** — a wedged CTP
  `Release()` ignores `SIGTERM`; `taskkill /F /PID <pid>`. Until it dies it locks
  `build\Release\ctp.node` (see build errors below).

---

## Build & install

| Symptom | Cause | Fix |
|---|---|---|
| `LNK1104: cannot open file ... ctp.node` | a wedged/zombie `node` process still holds the addon | find and `taskkill /F` the stray node process, then rebuild. |
| `fatal error LNK1103: 调试信息损坏` | stale/corrupt build object (e.g. interrupted/parallel build) | `npx node-gyp rebuild` (clean rebuild). |
| `MODULE_NOT_FOUND: node-addon-api` on a consumer install | source build with the build deps missing | fixed — `node-addon-api`/`node-gyp` are runtime `dependencies`; on an unshipped platform a C++ toolchain + Python are still required. |
| `.node` won't load on macOS arm64 / linux arm64 | no prebuild for that platform | it builds from source on install; ensure a toolchain is present. |
| C4819 codepage warning during build | Chinese comments in `.cc` under a GBK code page | harmless; ignore. |

---

## Error-code reference

**Send-return codes** (the value `reqXxx` passes back; for resolve-on-submit
methods like `reqOrderInsert` a non-zero one rejects the Promise):

| Code | Meaning |
|---|---|
| `0` | accepted / sent |
| `-1` | network send failed (not connected, or CTP refused the send) |
| `-2` | too many unhandled requests in flight |
| `-3` | per-second send rate exceeded (CTP flow control) |
| `-10001` | blocked by this binding's pre-trade risk (`lastRiskReason()` / message has the cause) |
| `-10002` | blocked by the rate limiter (`maxOrdersPerSec`) |
| `-10003` | blocked by the position **margin** cap (`riskSet({ maxMargin })`, the Σ price×vol×multiplier×marginRate cap — `maxPositionCost` is a deprecated alias; or per-instrument `setMaxPositionCost`) |
| `-10004` | blocked by a position-**volume** cap (`setMaxPosition`) |

**Exchange/broker errors** arrive as `rspInfo` (`{ errorId, errorMsg }`) on the
relevant event, and on a rejected request Promise as `err.errorId` /
`err.errorMsg`. `errorId 0` = success; non-zero carries a Chinese `errorMsg`
(GB18030-decoded for you) — the message is the reliable, human-readable cause, so
log it. Common ones are in the [order-rejection](#order-rejections) and
[login](#login--authentication) tables above.

```ts
try { await td.reqUserLogin({ brokerId: "9999", userId: "id", password: "bad" }); }
catch (e) { console.error(`CTP ${e.errorId}: ${e.errorMsg}`); }
```
