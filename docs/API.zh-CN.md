<!-- LANG-SWITCH -->
[English](API.md) · **简体中文**

# ctp-node API 参考

`ctp-node` 全部公开 TypeScript/JavaScript 接口的完整使用文档。架构与原生钩子内部细节见
[native-hooks.zh-CN.md](native-hooks.zh-CN.md)；设计总览见 [README](../README.md)。

- [安装](#安装)
- [快速开始](#快速开始)
- [核心概念](#核心概念)
- [`MarketData`](#marketdata) — 行情客户端
- [`Trader`](#trader) — 交易客户端
- [`CtpClient`](#ctpclient) — 公共基类（事件、关闭、背压）
- [类型](#类型) — `RiskLimits`、`InstrumentPositionLimit`、`SessionOptions`、`ArmSpec`、`ArmHandle`、`CallbackOptions`、`RspInfo`、`MdLoginReq`
- [枚举与结构体类型](#枚举与结构体类型)
- [风控一览](#风控一览)
- [实战示例](#实战示例) — 策略骨架 · 重连处理 · 组合下单

```ts
import {
  MarketData, Trader,
  Direction, OffsetFlag, OrderPriceType, // 生成的枚举
  type RiskLimits, type SessionOptions, type DepthMarketData,
} from "@hitrading/ctp-node";
```

---

## 安装

```bash
npm install @hitrading/ctp-node
```

支持的 Node.js 版本线为 `^22.22.2 || ^24.15.0 || >=26.0.0`，与内置 node-gyp 13 的要求一致。
npm 包提供 win32-x64 与 linux-x64 预编译二进制；macOS 及其他平台在安装时从源码编译，需要
C++ 工具链与 Python。

---

## 快速开始

### 行情

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

// 保持进程存活；关闭时调用一次 md.close()
```

### 带盘前风控的交易

```ts
import { Trader, Direction, OffsetFlag } from "@hitrading/ctp-node";

const td = new Trader("./flow/td/", "tcp://182.254.243.31:30002");

td.setRiskLimits({ maxOrderVolume: 10, maxOrdersPerSec: 20 });
td.setInstrumentPositionLimit("rb2510", 100); // 单边持仓不超过 100 手

td.on("front-connected", async () => {
  await td.session({
    brokerId: "9999", userId: "your-id", password: "your-pw",
    appId: "your-app", authCode: "your-auth-code",
  });

  // 提交成功即 resolve；成交通过 rtn-trade 到达
  await td.reqOrderInsert({
    instrumentId: "rb2510",
    direction: Direction.Buy,        // "0"
    combOffsetFlag: OffsetFlag.Open, // "0"
    limitPrice: 3500,
    volumeTotalOriginal: 1,
  }).catch((e) => console.error("报单被拒:", e.message));
});

td.on("rtn-trade", (t) => console.log("已成交", t.instrumentId, t.price, t.volume));
```

---

## 核心概念

**事件 + Promise 混合 API。** 流式推送（行情、报单/成交回报、连接状态）以 **EventEmitter**
事件的形式投递，使用 kebab-case 名称（`rtn-depth-market-data`、`rtn-order` 等）。请求/响应类
调用返回一个与响应关联的 **Promise**（`login()`、`reqQry*()` 等）。
响应事件在错误或空的最终行上可能没有 payload；解引用前请检查 `options.rspInfo` / `options.isLast`。

**严格请求对象。** 请求/配置必须是对象字面量（或无原型字典），且字段必须是显式自有字段。未知、
继承、拼错、已删除、Symbol 字段以及 `null` 字段值都会在请求到达 CTP 前直接报错，不会被静默编码为零。
输入数组必须稠密且每项都是显式自有值；稀疏或由原型提供的项会被拒绝。

**一次创建，进程内复用。** `MarketData` / `Trader` 只构造一次并一直持有。CTP 会自行重连——断线
后应**原地**处理：在 `front-connected` 再次触发时（首次连接 *以及* 每次自动重连都会触发）重新
跑一遍握手。**不要**销毁再重建客户端：Windows 下反复 create/close 已观察到会卡在厂商 DLL 的
`Release()`。关闭时调用一次 `close()`（或干脆不调用——进程退出会清理）。详见
[native-hooks.zh-CN.md → 进程生命周期](native-hooks.zh-CN.md#进程生命周期-一次创建并复用)。

**背压。** 行情溢出使用有界 keep-newest 暂存并保留最新行情，不再覆盖 JS 正在读取的槽位。交易端
在 4096 槽满后进入有序无损队列，不主动丢弃报单/成交/查询回报；持续消费不及时会以增长内存为代价。

**盘前风控在 C++ 中执行。** 所有风控限制（`setRiskLimits`、`setInstrumentPositionLimit`、`reduceOnly` / `haltAll` 等）都在原生
插件内、报单发送路径上、抵达 CTP 之前就执行——没有 JS 往返。被拦截的报单会让
`reqOrderInsert()` reject，原因在错误消息里。

> 完整的实战示例（策略骨架、重连处理、组合下单）见文末的[实战示例](#实战示例)。

---

## `MarketData`

行情客户端。订阅深度行情并以事件形式流式推送。

### `new MarketData(flowPath, fronts)`

| 参数 | 类型 | 含义 |
|---|---|---|
| `flowPath` | `string` | CTP 存放流文件的目录（缓存序列状态）。不存在会自动创建；每个客户端用各自的路径，如 `"./flow/md/"`。 |
| `fronts` | `string \| string[]` | 一个或多个前置地址，如 `"tcp://182.254.243.31:30012"`。空字符串/空数组会抛错。 |

构造会异步连接；先挂好你的处理函数，然后在 `front-connected` 处理函数里行动。

```ts
// 单个前置
const md = new MarketData("./flow/md/", "tcp://182.254.243.31:30012");

// 多个前置做故障切换
const md2 = new MarketData("./flow/md/", [
  "tcp://182.254.243.31:30012",
  "tcp://182.254.243.31:30011",
]);
```

### `md.login(req?)` → `Promise<RspUserLogin | undefined>`

登录行情前置。成功时以登录响应 resolve，失败时以 CTP 错误 reject（`err.errorId`、`err.errorMsg`）。

| `req` 字段（`MdLoginReq`，均可选） | 类型 | 含义 |
|---|---|---|
| `brokerId` | `string` | 经纪商代码。 |
| `userId` | `string` | 投资者/用户代码。 |
| `password` | `string` | 密码。 |
| `tradingDay` | `string` | 交易日（一般无需）。 |
| `userProductInfo` | `string` | 产品信息标识。 |

> SimNow 行情前置接受匿名 `login({})`；实盘前置需要凭证。
>
> 每次行情 `login()` / `logout()` 都必须完成后再开始下一次。部分前置会把这两类回包的
> `requestId` 固定为 0，因此 binding 会主动拒绝重叠的会话切换。若一次请求超时，原生行情缓存与
> 预埋单路径会保持 fail-closed：先忽略第一条迟到的最终回包（或等待前置状态变化重置），之后才允许
> 建立新的已认证缓存 epoch。

```ts
// 带凭证
md.on("front-connected", async () => {
  const rsp = await md.login({ brokerId: "9999", userId: "id", password: "pw" });
  console.log("已登录，交易日", rsp.tradingDay);
  md.subscribe(["rb2510"]);
});

// 匿名（SimNow 行情前置）
await md.login();

// 处理登录失败
try {
  await md.login({ brokerId: "9999", userId: "id", password: "wrong" });
} catch (e) {
  console.error("登录失败", e.errorId, e.errorMsg);
}
```

### `md.logout(req?)` → `Promise<UserLogout | undefined>`

登出。`req` 可带 `{ brokerId?, userId? }`。

与 `login()` 一样，行情登录/登出切换不能重叠；必须先等待返回的 Promise 完成。

```ts
await md.logout();
// 或指定账户
await md.logout({ brokerId: "9999", userId: "id" });
```

### `md.reqQryMulticastInstrument(req?)` → `Promise<MulticastInstrument[]>`

查询多播合约，并将全部应答行以数组返回。请求可包含 `topicId` 和
`instrumentId`。调用通过每个客户端独立的保守查询调度器串行发送，两次发送至少
间隔 1100 ms，调用方无需再手写 sleep。

```ts
const instruments = await md.reqQryMulticastInstrument({ topicId: 1 });
```

### `md.subscribe(instrumentIds)` → `number`

订阅深度行情。返回 CTP 发送码（`0` = 已接受，非零 / `-1` = 失败）。行情以
`rtn-depth-market-data` 事件到达。

| 参数 | 类型 | 含义 |
|---|---|---|
| `instrumentIds` | `string[]` | 要订阅的合约代码，如 `["rb2510", "au2508"]`。 |

```ts
md.on("rsp-sub-market-data", (info) => console.log("已订阅", info.instrumentId));
md.on("rtn-depth-market-data", (t) => console.log(t.instrumentId, t.lastPrice));

const rc = md.subscribe(["rb2510", "au2508"]);
if (rc !== 0) console.warn("订阅发送失败:", rc);

// 之后再追加订阅（例如换月到新合约）
md.subscribe(["rb2601"]);
```

### `md.unsubscribe(instrumentIds)` → `number`

退订深度行情。形参同 `subscribe`。

```ts
md.unsubscribe(["au2508"]);           // 退订一个
md.unsubscribe(["rb2510", "au2508"]); // 退订多个
```

### `md.subscribeForQuote(instrumentIds)` / `md.unsubscribeForQuote(instrumentIds)` → `number`

订阅 / 退订询价通知，以 `rtn-for-quote` 事件投递（期权做市常用）。

```ts
md.on("rtn-for-quote", (q) => console.log("收到询价", q.instrumentId));
md.subscribeForQuote(["IO2508-C-3900"]);
// ……稍后
md.unsubscribeForQuote(["IO2508-C-3900"]);
```

### `md.snapshot(instrumentId)` → `DepthMarketData | null`

某合约最新的完整深度 tick，**同步**地从 C++ 末值缓存读取。成功 MD 登录会开启一个全新的空缓存
epoch；只有此后属于当前认证会话的 tick 才会在抵达 JS 前更新它。连接/断线、新登录或登出尝试、
成功登出和关闭都会立即使该 epoch 失效并清空。非活动 epoch、尚无该合约 tick 或退订后返回 `null`。

| 参数 | 类型 | 含义 |
|---|---|---|
| `instrumentId` | `string` | 合约代码，如 `"rb2510"`。 |

```ts
md.subscribe(["rb2510"]);
// ……稍后，在代码任意位置——无需事件处理函数
const tick = md.snapshot("rb2510");
if (tick) console.log("中间价", (tick.bidPrice1 + tick.askPrice1) / 2, "@", tick.updateTime);
```

### `md.last(instrumentId)` → `number`

某合约的最新价，**同步**地从同一 C++ 缓存读取。若缓存 epoch 尚未认证/已经失效、还没有任何 tick，
或缓存项已清除（见 [`snapshot`](#mdsnapshotinstrumentid--depthmarketdata--null)），则返回 `0`。

```ts
const px = md.last("rb2510");
if (px > 0) console.log("最新成交价", px);
```

### `md.attachArm(trader)` → `void`

把这路行情的 tick 路由到某个 `Trader` 的预埋触发器（见
[`Trader.arm`](#tdarmmd-spec--armhandle)）。通常你调用 `td.arm(md, …)`，它会替你调用本方法。
一条行情最多服务一个**仍存活**的 Trader；第二个存活 Trader 会被拒绝，但旧 Trader 关闭后可以安全替换，
且不会由行情对象继续强引用已关闭的 JS 对象。
一路 `MarketData` 只服务于一个 `Trader` 的触发器；再 attach 另一个不同的 `Trader` 会抛错。

```ts
// 显式调用（一般不需要——td.arm() 会自动做）：
md.attachArm(td);

// attach 第二个不同且仍存活的 trader 会抛错：
md.attachArm(td);        // ok
md.attachArm(otherTd);   // 抛错：已在服务另一个 Trader
```

### `md.apiVersion` / `md.tradingDay` → `string`（只读）

CTP API 版本字符串，以及当前交易日（`YYYYMMDD`），连接后可用。

```ts
console.log("CTP 行情 API", md.apiVersion); // 如 "6.7.2"
md.on("front-connected", () => console.log("交易日", md.tradingDay));
```

### `md.droppedRecords` → `number`（getter）

keep-newest 背压下被淘汰的 tick 总数。监控它以发现消费慢的情况。

```ts
setInterval(() => {
  if (md.droppedRecords > 0) console.warn("行情已丢弃", md.droppedRecords, "条 tick");
}, 5000);
```

### `md.close()` → `void`

释放底层 CTP API 并释放原生资源。幂等。关闭时调用一次。见[生命周期](#核心概念)。

```ts
process.on("SIGINT", () => { md.close(); process.exit(0); });
```

### 行情事件

用 `md.on(name, handler)` 订阅。处理函数收到 `(data, options)`，其中 `options` 是
[`CallbackOptions`](#callbackoptions)。符号名在 `MarketDataEvent` 枚举里；普通字符串也行。

| 事件 | `data` 类型 | 触发时机 |
|---|---|---|
| `front-connected` | `undefined` | 已连接（首次连接和每次重连）。在此重新跑 login/subscribe。 |
| `front-disconnected` | `number`（原因码） | 连接断开。在途请求会 reject。 |
| `rsp-user-login` | `RspUserLogin` | 登录响应。 |
| `rsp-user-logout` | `UserLogout` | 登出响应。 |
| `rsp-qry-multicast-instrument` | `MulticastInstrument` | 多播合约查询的一行响应。 |
| `rsp-sub-market-data` | `SpecificInstrument` | 订阅应答。 |
| `rsp-unsub-market-data` | `SpecificInstrument` | 退订应答。 |
| `rtn-depth-market-data` | `DepthMarketData` | 一条深度行情 tick。 |
| `rtn-for-quote` | `ForQuoteRsp` | 一条询价通知。 |
| `rsp-error` | `undefined` | 一条 CTP 错误响应（`options.rspInfo`）。 |
| `error` | `unknown` | 你注册的某个*处理函数*抛了异常——见[错误处理](#error-事件)。 |

```ts
import { MarketDataEvent } from "@hitrading/ctp-node";

// 符号名
md.on(MarketDataEvent.RtnDepthMarketData, (t) => {
  const mid = (t.bidPrice1 + t.askPrice1) / 2;
});

// 重连处理
md.on("front-connected", async () => { await md.login(); md.subscribe(["rb2510"]); });
md.on("front-disconnected", (reason) => console.warn("行情断开，码", reason));
```

---

## `Trader`

交易客户端：报单、查询、持仓/风控跟踪，以及延迟敏感的预埋触发器。继承
[`CtpClient`](#ctpclient)；所有生成的 `reqXxx` 请求方法都可用（见
[请求方法](#生成的请求方法)）。

### `new Trader(flowPath, fronts)`

参数同 [`MarketData`](#new-marketdataflowpath-fronts)。Trader 会记住登录响应里的凭证，因此
`sync*` 无需传参；并会把自动 `OrderRef` 计数器播种到经纪商 `maxOrderRef` 之上，使 ref 永不与上
一会话冲突。

```ts
const td = new Trader("./flow/td/", "tcp://182.254.243.31:30002");
// 故障切换前置：
const td2 = new Trader("./flow/td/", ["tcp://182.254.243.31:30002", "tcp://182.254.243.31:30001"]);
```

### `td.session(opts)` → `Promise<{ identity, multipliers, marginRates, positions, orders }>`

连接后的一站式握手：**认证 → 登录 → 确认结算单 → 同步乘数 / 可选保证金率 / 报单 / 持仓**。在 `front-connected`
处理函数里调用（重连后再调一次）。返回经请求关联确认的
`identity: { frontId, sessionId, maxOrderRef }` 以及各同步步骤的行数。其中 `maxOrderRef`
是本次登录响应报告的值；Trader 的自动分配器可能保留更高的进程内高水位。每一步只是包装了对应的请求/同步方法，
所以需要更细控制时你也可以自己手写流程。

握手一开始，持仓账本和工作单账本就会被标记为不确定，使所有开仓请求在潜在重连缺口期间
fail-closed。成功的 `syncPositions()` 与 `syncOrders()` 只清除各自的不确定状态；若关闭其中某项
同步，对应闸门会一直 fail-closed，直到之后完成该项对账。

新建 Trader 从首次 `front-connected` 回调到达前就处于同样的 fail-closed 状态。

`opts` 是 [`SessionOptions`](#sessionoptions)：

登录回报必须包含明确的 `frontId` / `sessionId`，且 `brokerId` / `userId`
必须与请求一致。即使柜台把空回报或身份不匹配的回报标记为成功，
`session()` 也会拒绝继续；沿用旧/零会话键或对账到其他账户都会让本地账本不安全。

| 字段 | 类型 | 含义 |
|---|---|---|
| `brokerId` | `string` | 经纪商代码（必填）。 |
| `userId` | `string` | 投资者代码（必填）。 |
| `password` | `string` | 密码（必填）。 |
| `appId` | `string?` | 终端认证的 app id。SimNow：`"simnow_client_test"`。 |
| `authCode` | `string?` | 终端授权码。SimNow：16 个零。两者都省略则跳过认证。 |
| `confirmSettlement` | `boolean?` | 登录后确认结算单（实盘账户必须确认，否则报单被拒）。默认 `true`。 |
| `sync` | `object?` | `{ multipliers?: boolean \| string[], marginRates?: { instrumentIds: string[], hedgeFlag: "1" \| "2" \| "3" \| "5" \| "6" \| "7" }, positions?: boolean, orders?: boolean }`。乘数/持仓/报单默认拉取；保证金表必须显式给出有限合约列表和套保标志，因为 CTP 每个合约需一次查询，且不同套保标志使用不同费率表。 |

```ts
// SimNow 完整握手
td.on("front-connected", async () => {
  const counts = await td.session({
    brokerId: "9999", userId: "id", password: "pw",
    appId: "simnow_client_test", authCode: "0000000000000000",
  });
  console.log("已同步", counts); // { identity, multipliers, marginRates, positions, orders }
});

// 把乘数同步限定到你交易的合约（冷启动更快）
await td.session({
  brokerId: "9999", userId: "id", password: "pw",
  sync: {
    multipliers: ["rb2510", "au2508"],
    marginRates: { instrumentIds: ["rb2510", "au2508"], hedgeFlag: "2" },
    positions: true,
    orders: true,
  },
});

// 跳过结算单确认（不需要的环境）
await td.session({ brokerId: "9999", userId: "id", password: "pw", confirmSettlement: false });
```

> 手写等价流程：`await td.reqAuthenticate(...)` → `await td.reqUserLogin(...)` →
> `await td.reqSettlementInfoConfirm(...)` → `await td.syncMultipliers()` → `await td.syncMarginRates([...])` →
> `await td.syncOrders()` → `await td.syncPositions()`。

### `td.reqOrderInsert(req?)` → `Promise<void>`

通过 C++ 盘前风控网关报单。**提交成功即 resolve**——CTP 对已接受的报单不返回成功应答，只有
`rtn-order` / `rtn-trade`（用 `orderRef` 关联）。只有发送本身被拒（风控网关、限频、或 CTP API
错误码）才会 **reject**。`orderRef` 留空会自动分配一个唯一的数字 ref。

普通报单要求已确认的登录身份，`brokerId` 和 `investorId` 会自动从当前会话补齐。
显式传入不一致的值会在编码前被拒绝，避免把当前账户的持仓/工作单快照
用到另一账户的报单上。

`req` 是 `Partial<InputOrder>`；你几乎总会设置的字段：

| 字段 | 类型 | 含义 |
|---|---|---|
| `instrumentId` | `string` | 合约，如 `"rb2510"`。 |
| `direction` | `Direction`（`"0"`/`"1"`） | `Buy` / `Sell`。 |
| `combOffsetFlag` | `string` | 必须恰好一个字符：`"0"` 开仓、`"1"` 平仓、`"3"` 平今……多腿标志会拒绝。 |
| `limitPrice` | `number` | 限价；必须 `> 0`。**不支持**市价/任意价单（`limitPrice: 0`）。 |
| `volumeTotalOriginal` | `number` | 数量（手，正 int32）。 |
| `orderRef` | `string?` | 留空则自动分配；显式值必须是 1–12 位十进制数字。 |
| `orderPriceType` | `OrderPriceType?` | 可选；若设置必须为 `"2"`（限价）。默认限价。 |

> **只接受单合约的 GFD、FAK 和 FOK 限价单。** GFD 使用 `timeCondition: "3", volumeCondition: "1"`，FAK 使用 `"1", "1"`，FOK 使用 `"1", "3"`。`reqOrderInsert` 会主动拒绝（抛 `TypeError`）本地风控预留台账无法表示的其它时间/成交量条件组合、最小成交量、触发、止损、互换、强平等变体，以及多腿的 offset/hedge 标志。

```ts
import { Direction, OffsetFlag } from "@hitrading/ctp-node";

// 1）限价买开 1 手
await td.reqOrderInsert({
  instrumentId: "rb2510", direction: Direction.Buy,
  combOffsetFlag: OffsetFlag.Open, limitPrice: 3500, volumeTotalOriginal: 1,
});

// 2）处理被拒（风控网关 / 限频 / CTP 错误）
try {
  await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 9999, volumeTotalOriginal: 1 });
} catch (e) {
  console.warn("被拒:", e.message); // 如 "blocked by pre-trade risk: order price deviates too far from reference"
}

// 3）用自己的 orderRef 关联成交
await td.reqOrderInsert({ orderRef: "42001", instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 2 });
td.on("rtn-trade", (t) => { if (t.orderRef === "42001") console.log("我的报单成交", t.volume); });

// 4）平今一笔空头（卖 -> 平）
await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseToday, limitPrice: 3490, volumeTotalOriginal: 1 });
```

### `td.reqOrderAction(req?)` → `Promise<void>`（撤单）

撤/改一笔挂单。提交成功即 resolve。Delete 在所有风控模式下始终保留；Modify 可能增大敞口，因此按
无法建模的变更处理，只有处于 `NORMAL` 且所有本地风控关闭时才允许。用 `orderRef` + `frontId` +
`sessionId`（取自其 `rtn-order`）或用 `exchangeId` + `orderSysId` 来定位报单。

```ts
import { ActionFlag } from "@hitrading/ctp-node";

let working;
td.on("rtn-order", (o) => { if (o.orderStatus === "3") working = o; }); // NoTradeQueueing（未成交挂单）

// 撤掉它
await td.reqOrderAction({
  instrumentId: working.instrumentId,
  orderRef: working.orderRef, frontId: working.frontId, sessionId: working.sessionId,
  actionFlag: ActionFlag.Delete, // "0"
});

// 改用交易所报单号撤单
await td.reqOrderAction({
  instrumentId: working.instrumentId,
  exchangeId: working.exchangeId, orderSysId: working.orderSysId,
  actionFlag: "0",
});
```

### 查询方法 — `td.reqQry*` / `td.reqQuery*`

每个 `reqQry*` 以**行数组** resolve（多行响应会累积；空结果 resolve `[]`）；两个单响应的
`reqQuery*` 方法 resolve 一行。所有生成的查询共享同一个每 Trader FIFO 调度器：最多一个查询在途，
两次尝试发送至少间隔 1100 ms；并发直接调用也无法绕过限频，因此不要再加 `setTimeout(1000)`。
请求参数会在调用时快照；断线、登录或登出会拒绝旧上下文中排队的查询。调度器按客户端实例隔离，
不是进程全局，且最多接受 256 个在途/排队查询。原始查询只限频、不自动重试；`sync*` 还会处理
瞬时 `-2`/`-3` 限流错误及必须存在但冷启动时暂为空的数据。

```ts
// 当前持仓
const positions = await td.reqQryInvestorPosition({ brokerId: "9999", investorId: "id" });
for (const p of positions) console.log(p.instrumentId, p.posiDirection, p.position);

// 账户资金
const [account] = await td.reqQryTradingAccount({ brokerId: "9999", investorId: "id" });
console.log("可用", account?.available);

// 合约详情（乘数、最小变动价位……）
const [rb] = await td.reqQryInstrument({ instrumentId: "rb2510" });
console.log("乘数", rb?.volumeMultiple, "tick", rb?.priceTick);

// 当日报单 / 成交
const orders = await td.reqQryOrder({ brokerId: "9999", investorId: "id" });
const trades = await td.reqQryTrade({ brokerId: "9999", investorId: "id" });
```

完整列表见[请求方法](#生成的请求方法)。

### `td.syncMultipliers(instrumentIds?)` → `Promise<number>`

触发合约查询，使 C++ 风控引擎取到合约乘数——它直接从 CTP 的 `OnRspQryInstrument` 响应里取，因此
本方法不再调用 JS setter。按乘数精确计算名义价值和持仓保证金限制所必需。不传参则一次请求查询全部
合约；传符号列表可限定范围。显式空数组是返回 `0` 的 no-op，只有省略参数才表示“查询全部”。会穿越
冷启动限频重试，返回最终回调行含可用乘数的合约数量。

合约刷新会在请求发出前先使旧乘数失效，因此失败/空刷新不会继续使用陈旧元数据。若账户已有持仓，
随后还应调用 `syncPositions()` 更新权威 `UseMargin`；在此之前金额型开仓闸门保持 fail-closed。

```ts
const n = await td.syncMultipliers();           // 全部合约
await td.syncMultipliers(["rb2510", "au2508"]);  // 仅这些
console.log(n, "个合约带有乘数");
```

### `td.syncMarginRates(instrumentIds, hedgeFlag?)` → `Promise<number>`

为有限交易合约集查询投资者表（套保标志默认 `"1"`，投机）；仅当返回 `IsRelative` 时才追加
交易所基础表查询，共享调度器会自动间隔发送。相对投资者表会与交易所基础表逐字段相加；任一缺失
仍 fail-closed。

刷新开始前会同时使旧投资者表和交易所基础表失效。已有持仓时，刷新费率后还应调用
`syncPositions()`；引擎不会把按旧费率估算的持仓保证金继续当作权威值。

### `td.syncPositions()` → `Promise<number>`

从 CTP（`reqQryInvestorPosition`，取每行的 `UseMargin`）拉取持仓**保证金**并播种到风控引擎的持仓
跟踪器。空快照必须连续确认两次；应用时用版本号原子替换并保留工作单预约，查询期间若有实时回报会
重试。始终使用已确认的登录身份，传入账户覆盖参数会被拒绝。返回播种的持仓数。

```ts
const held = await td.syncPositions();
console.log("已播种", held, "个持仓，保证金 =", td.accountMargin);
```

### `td.syncOrders()` → `Promise<number>`

从 CTP 当前挂单（`reqQryOrder`）重建在途预约——一次权威重同步。登录后和任何重连后都应调用，
使持仓上限把已在经纪商挂着的报单也算进去（重连前下的，或——因为 CTP 也会投递——同账户另一终端
下的）。`a`/`b`/`c` 条件单状态同样保留预约；应用快照时校验实时账本版本。先查询乘数和相关绝对
保证金表。返回重新预约的挂单开仓单数。

如果账户中存在仍在工作的多腿/不支持订单（包括其他终端下的单），单腿账本无法表达其风险敞口。
快照仍会原子提交并记录这类订单，然后本方法抛错；在后续一次成功的 `syncOrders()` 确认它已消失前，
所有开仓请求都会 fail-closed。

`syncOrders()` 还会主动把持仓侧重新标为不确定，因此必须在它**之后**运行 `syncPositions()`；即使调用者
把顺序写反，也只会保持 fail-closed，不会在两个缺少事务共同切点的经纪商快照上恢复开仓。

```ts
await td.syncMultipliers();
const working = await td.syncOrders();
await td.syncPositions();
console.log("重新预约了", working, "笔挂单");
```

### 风控配置

#### `td.setRiskLimits(config)` → `this`

把盘前风控限制发布给 C++ 执行器（立即生效）。非有限值（`NaN`/`Infinity`）会被**抛错拒绝**，而不
是静默关闭该控制。`config` 是 [`RiskLimits`](#risklimits)，会整体替换上一份配置；控制字段省略或传
仅 `0` 表示关闭；负数会被拒绝。`maxReferenceAgeMs` 例外：必须为正数，省略时恢复默认 3000 ms。
未知字段（包括已删除的旧名称 `maxMargin`）会直接报错，不会被静默忽略。

```ts
// 全套
td.setRiskLimits({
  maxOrderVolume: 10,          // 单笔 ≤ 10 手
  maxPriceDeviation: 0.02,     // 距参考价 ≤ 2%
  maxNotional: 5_000_000,      // Futures/TAS 单笔开仓名义价值 ≤ 5M
  maxOrdersPerSec: 20,         // 令牌桶限频
  orderBurst: 40,              // 桶容量（默认 = maxOrdersPerSec）
  maxCloseOrdersPerSec: 10,    // 可选：独立平仓限频
  closeOrderBurst: 20,
  maxAccountMargin: 20_000_000, // 全账户已持仓 + 工作单预约保证金 ≤ 20M
});

// 只设几项（其余保持关闭）
td.setRiskLimits({ maxOrderVolume: 5, maxOrdersPerSec: 10 });

// 传 0 关闭某控制
td.setRiskLimits({ maxNotional: 0 });
```

#### `td.reduceOnly()` / `td.haltAll()` / `td.resume()` → `this`

原子模式为 `NORMAL`、`REDUCE_ONLY`、`HALT_ALL`。`reduceOnly()` 拦截普通开仓、全部高级变更请求和
银期转账，但保留撤单与单腿普通平仓；`haltAll()` 还会拦截平仓报单、清空全部 arm，并在 `resume()`
前拒绝新建 arm。
模式切换与最终 CTP API 发送共用同一屏障，因此切换返回后不会再漏出被禁止的 armed 报单。
`resume()` 恢复 `NORMAL`。复杂 insert、对冲设置、套利/套保确认无法进入普通预约账本，只要配置任一
本地风控也会 fail-closed。生成器要求未来新增的每个非查询 SDK 请求先完成分类；未审核方法会让
代码生成失败，而不是默认无守卫放行。

这种隔离在实例生命周期内是单向且粘滞的：一旦 CTP 在纯 binding 模式接受过任何无法建模的增险请求，
之后再启用任一本地风控，普通开仓也会 fail-closed。`syncOrders()` 只覆盖普通单腿报单，不能证明
quote/exec/comb/parked 风险已经消失。同账户其他终端的高级变更或银期转账实时回报一旦被本实例观察到，
也会设置同一个粘滞的 fail-closed 标记；不能因为请求并非由本进程发起就把它误当成普通账本状态。
一个 Trader 实例应在整个生命周期内固定使用一种 capability 模式，不要在放行高级请求后再切换到
本地风控交易。

银期转账会改变可用担保资金，却不在本地仓位/保证金账本中建模。因此即使处于 `NORMAL`，只要启用了
任一本地风控也会拒绝；仅在所有本地风控关闭的协议 binding 模式下放行。调用方仍须自行承担鉴权与
转账授权。

```ts
// 停止增险，但保留平仓和撤单
process.on("SIGINT", () => td.reduceOnly());

// 开仓和平仓都停止（撤单仍可用）
td.haltAll();

// 评估情况后重新启用交易
td.resume();

// 例：PnL 突破限额则熔断
td.on("rtn-trade", () => { if (computePnl() < -100000) td.reduceOnly(); });
```

#### `td.setInstrumentPositionLimit(instrumentId, limit)` / `td.setInstrumentPositionLimits(limits)` → `this`

按合约限制开仓持仓（手数），在每笔开仓单上执行。`limit` 是 [`InstrumentPositionLimit`](#instrumentpositionlimit)：一个数字同时限制
两边；`{ long, short }` 分别限制各边。在 `{ long, short }` 内，**省略某一边会让该边的当前上限保持
不变**（传 `0` 清除；负数会被拒绝）。多空独立跟踪。校验按 committed = 已持仓 + 在途（挂单）量。

```ts
td.setInstrumentPositionLimit("rb2510", 100);                  // 多 ≤ 100 且空 ≤ 100
td.setInstrumentPositionLimit("au2508", { long: 50, short: 10 }); // 不对称

// 只抬高多头上限，空头保持原样
td.setInstrumentPositionLimit("au2508", { long: 80 });
// 清除空头上限
td.setInstrumentPositionLimit("au2508", { short: 0 });

// 批量
td.setInstrumentPositionLimits({ rb2510: 100, ru2510: { long: 100, short: 20 }, au2508: 10 });
```

#### `td.setInstrumentMarginLimit(instrumentId, limit)` / `td.setInstrumentMarginLimits(limits)` → `this`

限制单个合约的开仓持仓**保证金**（价 × 量 × 乘数 × 按金额 + 量 × 按手，多空相加）。按合约计，
与账户级 `setRiskLimits({ maxAccountMargin })` 相互独立；两者同时生效。传
`limit = 0` 移除，负数会被拒绝。启用金额上限前必须查询到有效乘数和绝对保证金表；缺失元数据会拒绝开仓。
计算同时包含按金额与按手两部分；`IsRelative` 表与 `syncMarginRates()` 获取的交易所基础表相加，
任一侧缺失则 fail-closed。

该公式只对 CTP 期货/TAS 产品类启用。期权、组合与现货需要产品专用保证金模型，在金额上限开启时
会 fail-closed。

`maxNotional` 使用同一 Futures/TAS allowlist。期权权利金或组合价格不会冒充期货名义价值；
产品类别未知/不支持时，启用的名义价值闸会 fail-closed。

名义价值与工作单保证金统一按 `max(委托价, 新鲜参考价)` 估值，避免卖开低限价低估敞口。必须通过
`trackMarketData()` 接入行情；参考价缺失或超过 `maxReferenceAgeMs` 时，所有已启用的金额闸都会
fail-closed。每次金额准入前还会按当前新鲜参考价重估既有工作单。

```ts
td.setInstrumentMarginLimit("au2508", 5_000_000); // 黄金保证金 ≤ 5M
td.setInstrumentMarginLimits({ ag2508: 2_000_000, au2508: 5_000_000 });
td.setInstrumentMarginLimit("au2508", 0);         // 移除上限
```

#### `td.trackMarketData(md)` → `this`

提供价格偏离、名义价值与工作单保证金校验所使用的实时参考价。把这路 `MarketData` 的 C++ 快照缓存直接接入本
Trader 的风控引擎，使偏离参考价**在 C++、于报单发送路径上**读取——无 JS 往返，且覆盖在 C++ 里
触发、JS 不在回路中的预埋单。未接入参考价或行情超过 `maxReferenceAgeMs`（默认 3000 ms）时，
已启用的偏离或金额校验都会 fail-closed。

```ts
td.setRiskLimits({ maxPriceDeviation: 0.02 });

// 从行情实时取参考价（每笔报单都在 C++ 读取，含预埋单）
td.trackMarketData(md);
```

> 合约乘数（用于名义价值 / 保证金计算）和按合约的保证金率（用于 `maxAccountMargin` 上限）**不**从 JS
> 喂入：C++ 风控引擎直接从 CTP 的 `OnRspQryInstrument` / `OnRspQryInstrumentMarginRate` 回调里
> 取。所以任何 `reqQryInstrument` 都会保持乘数最新（见
> [`syncMultipliers`](#tdsyncmultipliersinstrumentids--promisenumber)），而对你交易的合约查询
> 应优先用 `syncMarginRates()` 覆盖实际交易的合约和套保标志：它同时查询投资者/交易所表，使用
> 按金额和按手字段，并正确合并 `IsRelative`。缺失元数据不会被猜测。

### 持仓保证金跟踪

#### `td.seedPositions(positions)` → `this`

用一份**完整**的手工对账快照原子替换已持仓。每行是
`{ instrumentId, side, hedgeFlag, volume, margin }`，其中 `margin` 是实际占用资金
（`InvestorPosition.UseMargin`）。空数组代表权威空仓。已删除的单项 `seedPosition()` 不保留：
多次增量 JS 调用之间会把半成品账本暴露给原生 arm 线程。

```ts
td.seedPositions([
  { instrumentId: "rb2510", side: "long", hedgeFlag: "1", volume: 3, margin: 30_000 },
  { instrumentId: "au2508", side: "short", hedgeFlag: "1", volume: 2, margin: 112_000 },
]);
```

#### `td.seedFromPositions(positions)` → `this`

从完整的 `reqQryInvestorPosition` 行集合原子替换真实开仓**保证金**（取每行的 `UseMargin`），并按
每行受支持的 `HedgeFlag` 保留独立成本桶，适用于**净持仓为分边模式（gross）**的账户
（`posiDirection` `"2"` = 多，`"3"` = 空）。净持仓、未知方向或未知投保标志会被拒绝，因为猜测会
破坏按边手数或保证金账本；请显式转换这类行，再一次提交完整的 `seedPositions()` 快照。

```ts
const rows = await td.reqQryInvestorPosition({ brokerId: "9999", investorId: "id" });
td.seedFromPositions(rows); // 完整已持仓快照；工作单预约保持不变
```

#### `td.accountMargin` → `number`（只读）

风控引擎当前跟踪的已持仓保证金；它有意不包含工作单预约，而 `maxAccountMargin` 的准入判断使用
**已持仓 + 已预约**。该属性仅用于 best-effort 可观测：元数据缺失或刷新期间它未必等于精确准入值；
金额闸启用时仍会 fail-closed，直到权威持仓同步完成。

```ts
console.log("账户保证金", td.accountMargin);
td.on("rtn-trade", () => console.log("当前保证金", td.accountMargin));
```

#### `td.resetPositions()` → `this`

只清空已持仓保证金，工作单预约会保留。reset 不能证明账户确实为空，因此所有开仓会继续 fail-closed，
直到 `seedFromPositions()` 或 `syncPositions()` 完成一次权威、带版本校验的原子替换。

```ts
td.resetPositions();              // 清空已持仓、保留预约；阻止开仓
td.seedFromPositions(freshRows);  // 原子提交完整快照；恢复账本
```

### 延迟敏感的预埋触发器

#### `td.arm(md, spec)` → `ArmHandle`

预埋一个完全在 C++、于行情回调线程上判定并触发的触发器——当 `md` 看到条件时，报单通过本 Trader
的风控网关发出，**无 JS 往返**。成功提交后立即从逐 tick 扫描中删除；reduce-only、限频、元数据
或同步发送拒绝会让它保留，并按 100ms 起步、5 秒封顶的指数退避重试。`haltAll()` 是硬停机例外：
它会关闭最终发送屏障并清空
全部已有 arm，之后 `resume()` 不会触发历史委托。回执通过普通事件到达。
每次 front 连接/断开事件以及 Trader 登录/注销请求也会清空所有 arm：
授权不会跨越认证会话边界。只有在 `session()` 完成新会话对账后才应重新预埋。

传入的 `md` 还会自动成为本 Trader 的新鲜参考价来源，供偏离、名义价值与工作单保证金校验使用。

`spec` 是 [`ArmSpec`](#armspec)：`{ instrumentId, side, triggerPrice, order }`。**buy** 在
`ask ≤ triggerPrice` 时触发；**sell** 在 `bid ≥ triggerPrice` 时触发。`order` 是完整的
`Partial<InputOrder>`，会被预先校验（必须有 `instrumentId`、`direction`、`combOffsetFlag` 和
`volumeTotalOriginal > 0`）。

返回一个 [`ArmHandle`](#armhandle)——调用 `handle.disarm()` 移除。

```ts
import { Direction, OffsetFlag } from "@hitrading/ctp-node";

// 止损：bid 一触及 3450 就卖出 rb2510
const stop = td.arm(md, {
  instrumentId: "rb2510",
  side: "sell",
  triggerPrice: 3450,
  order: {
    instrumentId: "rb2510", direction: Direction.Sell,
    combOffsetFlag: OffsetFlag.CloseToday, limitPrice: 3445, volumeTotalOriginal: 1,
  },
});

// 突破入场：ask 触及 3600 时买入
const entry = td.arm(md, {
  instrumentId: "rb2510", side: "buy", triggerPrice: 3600,
  order: { instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3605, volumeTotalOriginal: 2 },
});

// 撤掉一个尚未触发的触发器
stop.disarm();
```

#### `td.armStats` → `{ fired: number; blocked: number }`（只读）

预埋触发器的可观测性（它们在 C++ 里触发，JS 不在回路中）：有多少触发并发出了（`fired`）vs 被
风控网关/发送拒绝了（`blocked`）。在你预期会触发的触发之后轮询它——被拦截的预埋单否则不可见。

```ts
const { fired, blocked } = td.armStats;
console.log(`预埋：${fired} 已发，${blocked} 被拒`);
if (blocked > 0) console.warn("有一笔预埋单被风控拒绝");
```

### `td.apiVersion` / `td.tradingDay` → `string`（只读）

CTP 交易 API 版本与当前交易日。

```ts
console.log("CTP 交易 API", td.apiVersion, "交易日", td.tradingDay);
```

### `td.close()` / `td.droppedRecords`

同 [`CtpClient`](#ctpclient)。交易端固定环满后进入有序无损队列，不主动丢弃报单/成交/查询回报。

```ts
console.log("交易端丢弃数（应保持 0）:", td.droppedRecords);
process.on("SIGINT", () => td.close());
```

### 交易事件

| 事件 | `data` 类型 | 触发时机 |
|---|---|---|
| `front-connected` | `undefined` | 已连接（首次连接和每次重连）。在此跑 `session()`。 |
| `front-disconnected` | `number` | 连接断开。在途请求会 reject。 |
| `rsp-user-login` | `RspUserLogin` | 登录响应。 |
| `rtn-order` | `Order` | 报单状态更新（已接受、排队中、已成、已撤等）。 |
| `rtn-trade` | `Trade` | 一笔成交。 |
| `err-rtn-order-insert` | `InputOrder` | 报单被交易所拒绝（`options.rspInfo`）。 |
| `error` | `unknown` | 你注册的某个*处理函数*抛了异常——见下。 |

另外每个请求方法都有对应的 `rsp-*` 事件（如 `rsp-qry-investor-position`），不过你通常通过请求
Promise 来消费它们。

```ts
td.on("rtn-order", (o, opts) => {
  console.log(o.instrumentId, "状态", o.orderStatus, "ref", o.orderRef);
});
td.on("rtn-trade", (t) => console.log("成交", t.instrumentId, t.price, "x", t.volume));
td.on("err-rtn-order-insert", (input, opts) => {
  console.error("交易所拒绝", input.orderRef, opts.rspInfo?.errorMsg);
});
```

---

## `CtpClient`

`MarketData` 和 `Trader` 的公共基类。你不会直接构造它，但它的成员在两个客户端上都可用。

### `client.on(event, handler)` → `this`

注册事件处理函数（标准 `EventEmitter`）。处理函数收到 `(data, options)`；`options` 是
[`CallbackOptions`](#callbackoptions)。所有事件名都是普通字符串（每个子类的类型化重载列出了常用
的那些）；未知的原生事件以 `event:<id>` 形式出现。

```ts
md.on("rtn-depth-market-data", (tick, options) => {
  if (options.rspInfo) console.error("错误记录", options.rspInfo.errorMsg);
  else console.log(tick.lastPrice);
});

// 用 .once 一次性，用 .off 移除——它就是个普通 EventEmitter
td.once("front-connected", () => console.log("首次连接"));
```

### `client.droppedRecords` → `number`（getter）

自构造起背压下丢弃的记录总数。行情端持续攀升说明消费跟不上；交易端使用无损溢出队列，计数保持 0。

```ts
setInterval(() => console.log("丢弃数:", md.droppedRecords, td.droppedRecords), 10_000);
```

### `client.close()` → `void`

释放底层 CTP API 并释放原生资源（解码环、SPI、后台线程）。幂等。在途请求 Promise 以
`"client closed"` reject。关闭时调用一次。

```ts
async function shutdown() { td.close(); md.close(); }
process.on("SIGTERM", shutdown);
```

### `error` 事件

你注册的某个事件处理函数里抛出的异常不能拖垮数据面，因此它会被捕获并异步重新抛出：

- 如果你订阅了 `error`，异常会投递到那里（可捕获）；
- 否则它会在下一个 tick 以 `uncaughtException` 重新抛出——这正是一个会抛异常的 `EventEmitter`
  监听器的正常归宿，只是延后了，使数据面保持一致。

无论哪种方式，环都会继续前进——一个有 bug 的处理函数绝不会导致重复投递或卡住记录。

```ts
md.on("error", (err) => console.error("一个行情处理函数抛了异常:", err));
td.on("error", (err) => console.error("一个交易处理函数抛了异常:", err));
```

### 生成的请求方法

每个 CTP 请求都映射为 `Trader` 上一个 camelCase 方法，接受 `Partial<…Field>` 并返回 Promise：

- **`reqQry*`**（查询）→ `Promise<unknown[]>`（行数组；空则 `[]`），统一经过共享查询调度器。
- **发往交易所的 insert/action**（`reqOrderInsert`、`reqOrderAction`、`reqExecOrderInsert`、
  `reqQuoteInsert`、`reqForQuoteInsert`、`reqOptionSelfCloseInsert`、`reqCombActionInsert`，以及
  它们的 `*Action` 撤单）→ `Promise<void>`，**提交成功即** resolve（CTP 不返回成功响应；结果
  通过 `rtn-*` 事件到达）。发送被拒则 reject。
- **其余所有请求** → `Promise<...>`，以单条 `OnRsp*` 响应行 resolve。

```ts
// 手动认证 / 登录 / 确认（session() 自动化的就是这些）
await td.reqAuthenticate({ brokerId: "9999", userId: "id", appId: "simnow_client_test", authCode: "0000000000000000" });
await td.reqUserLogin({ brokerId: "9999", userId: "id", password: "pw" });
await td.reqSettlementInfoConfirm({ brokerId: "9999", investorId: "id" });

// 结算单文本
const [info] = await td.reqQrySettlementInfo({ brokerId: "9999", investorId: "id" });
```

常用的：`reqAuthenticate`、`reqUserLogin`、`reqUserLogout`、`reqSettlementInfoConfirm`、
`reqQrySettlementInfo`、`reqQryInstrument`、`reqQryInvestorPosition`、
`reqQryInvestorPositionDetail`、`reqQryTradingAccount`、`reqQryOrder`、`reqQryTrade`、
`reqOrderInsert`、`reqOrderAction`。（完整集合由 CTP 头文件生成——共 125 个请求方法。）

---

## 类型

### `RiskLimits`

[`setRiskLimits`](#tdsetrisklimitsconfig--this) 的盘前风控限制。所有字段可选；控制字段**省略或 `0` =
关闭**。`maxReferenceAgeMs` 必须为正数，省略时为 3000 ms；`NaN`/`Infinity` 抛错。

| 字段 | 类型 | 含义 |
|---|---|---|
| `maxOrderVolume` | `number` | 单笔最大手数。 |
| `maxPriceDeviation` | `number` | 最大 `\|价 − 参考价\| / 参考价` 比例；参考价缺失或过期时 fail-closed。 |
| `maxReferenceAgeMs` | `number` | 偏离与金额估值允许的最大行情年龄；默认 3000 ms。 |
| `maxNotional` | `number` | 单笔最大名义价值（`max(委托价, 新鲜参考价) × 量 × 乘数`）；参考价缺失/过期时 fail-closed。 |
| `maxOrdersPerSec` | `number` | 每秒最大开仓报单发送数（令牌桶）。 |
| `orderBurst` | `number` | 令牌桶容量。默认：`maxOrdersPerSec`。 |
| `maxCloseOrdersPerSec` | `number` | 独立平仓发送速率；默认关闭。 |
| `closeOrderBurst` | `number` | 平仓令牌桶容量；默认 `maxCloseOrdersPerSec`。 |
| `maxAccountMargin` | `number` | 账户已持仓 + 工作单预约保证金上限。计算按金额和按手；相对投资者表与交易所基础表相加。新鲜参考价、乘数或任一所需表缺失时 fail-closed。 |

```ts
const conservative: RiskLimits = { maxOrderVolume: 5, maxOrdersPerSec: 10, maxNotional: 2_000_000 };
td.setRiskLimits(conservative);
```

### `InstrumentPositionLimit`

`number | { long?: number; short?: number }` ——
[`setInstrumentPositionLimit`](#tdsetinstrumentpositionlimitinstrumentid-limit--tdsetinstrumentpositionlimitslimits--this) 的上限参数。
一个数字同时限制两边；`{ long, short }` 分别限制（省略某边 = 不变，`0` = 清除，负数拒绝）。

```ts
const a: InstrumentPositionLimit = 100;                    // 两边都 ≤ 100
const b: InstrumentPositionLimit = { long: 50, short: 5 }; // 多 ≤ 50，空 ≤ 5
```

### `SessionOptions`

[`session`](#tdsessionopts--promise-identity-multipliers-marginrates-positions-orders-) 的选项。字段见那里的表格。

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
  instrumentId: string;          // 触发器监视的合约
  side: "buy" | "sell";          // buy: ask ≤ trigger 时触发；sell: bid ≥ trigger 时触发
  triggerPrice: number;          // 必须 > 0
  order: Partial<InputOrder>;    // 触发时发出的报单（需 instrumentId、direction、
                                 // combOffsetFlag、volumeTotalOriginal > 0）
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
  disarm(): boolean; // 移除触发器；若已消失/已触发则返回 false
}
```

```ts
const handle = td.arm(md, spec);
console.log("预埋 id", handle.id);
if (!handle.disarm()) console.log("它已经触发过了");
```

### `CallbackOptions`

每个事件处理函数的第二个参数：

```ts
interface CallbackOptions {
  requestId?: number;   // 本响应关联的请求（仅响应）
  isLast?: boolean;     // 多行响应的最后一行（仅响应）
  rspInfo?: RspInfo;    // 当记录带有 CTP 错误时存在
}
```

```ts
td.on("rsp-qry-investor-position", (row, options) => {
  if (options.rspInfo) return console.error(options.rspInfo.errorMsg);
  collect(row);
  if (options.isLast) finish(); // 多行响应的最后一行
});
```

### `RspInfo`

```ts
interface RspInfo {
  errorId: number;      // 0 = 成功
  errorMsg: string;     // 可读（已 GB18030 解码）
}
```

被 reject 的请求 Promise 的 error 也带这些字段（`err.errorId` / `err.errorMsg`）：

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

## 枚举与结构体类型

所有 CTP 枚举（值）和结构体接口（类型）都由 CTP 头文件生成，并从包根目录重新导出。

**枚举**是字符串值，既可用枚举成员也可用原始码：

```ts
import { Direction, OffsetFlag, OrderStatus, OrderPriceType, ActionFlag } from "@hitrading/ctp-node";

Direction.Buy;          // "0"
Direction.Sell;         // "1"
OffsetFlag.Open;        // "0"
OffsetFlag.CloseToday;  // "3"
OrderStatus.AllTraded;  // "0"
ActionFlag.Delete;      // "0"

// 两种写法在结构体字段上都能通过类型检查：
const a = { direction: Direction.Buy };
const b = { direction: "0" } as const;
```

**结构体类型**描述事件载荷和请求字段的形状——如 `DepthMarketData`（一条 tick）、`Order`、
`Trade`、`InputOrder`、`InvestorPosition`、`RspUserLogin`。作为类型导入：

```ts
import type { DepthMarketData, Order, Trade, InputOrder } from "@hitrading/ctp-node";

md.on("rtn-depth-market-data", (tick: DepthMarketData) => {
  const mid = (tick.bidPrice1 + tick.askPrice1) / 2;
});

function buildOrder(): Partial<InputOrder> {
  return { instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 1 };
}
```

> 字段名是 CTP 字段的 camelCase 版本（`LastPrice` → `lastPrice`，`BidPrice1` → `bidPrice1`）。
> CTP 会用一个哨兵值（`Number.MAX_VALUE`）填充未设置的数值价格字段；如果你读取一个可能尚未成交
> 的价格，请对此做防护（风控引擎已经做了）。

---

## 风控一览

| 控制 | 设置方式 | 范围 | 关闭条件 |
|---|---|---|---|
| 风控模式 | `reduceOnly()` / `haltAll()` / `resume()` | 开仓或全部报单；`haltAll()` 同时清空 arm | `resume()` |
| 单笔最大手数 | `setRiskLimits({ maxOrderVolume })` | 每笔 | `0` / 省略 |
| 最大价格偏离 | `setRiskLimits({ maxPriceDeviation })` + 参考价 | 每笔 | `0` / 省略（无参考价会拦截） |
| 最大名义价值 | `setRiskLimits({ maxNotional })` | 每笔 | `0` / 省略 |
| 开仓限频 | `setRiskLimits({ maxOrdersPerSec, orderBurst })` | 每秒 | `0` / 省略 |
| 平仓限频 | `setRiskLimits({ maxCloseOrdersPerSec, closeOrderBurst })` | 每秒 | `0` / 省略 |
| 最大持仓手数 | `setInstrumentPositionLimit(id, …)` | 每合约、每边 | `0` |
| 最大持仓保证金（按合约） | `setInstrumentMarginLimit(id, …)` | 每合约 | `0` |
| 最大持仓保证金（全账户） | `setRiskLimits({ maxAccountMargin })` | 全账户 | `0` / 省略 |

持仓手数和持仓保证金上限都按 **committed = 已持仓（已成）+ 在途（挂单）量** 计算，因此一连串
开仓不会在成交回报到达前溜过上限。用 `syncPositions()` / `seedPositions()` 喂入已持仓，用
`syncOrders()` 喂入挂单（尤其是重连后）。启用金额上限前先查询乘数及投资者/交易所保证金表；
缺失元数据会拦截受限开仓。所有上限都
在 C++ 发送路径上执行；突破会让 `reqOrderInsert()` reject，原因在消息里。

---

## 实战示例

把上面那些单独的调用串成可运行整体的端到端模式。

### 一个完整的策略骨架

一个最小但具备生产形态的骨架：风控先行、连接时握手、持仓以成交为准（唯一真相来源）、优雅关闭。
把你自己的逻辑放进 `signal()` 里。

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

// 1) 在任何报单能发出之前先配置风控（在 C++ 中执行）。
td.setRiskLimits({ maxOrderVolume: 5, maxNotional: 2_000_000, maxOrdersPerSec: 10, maxPriceDeviation: 0.02, maxAccountMargin: 5_000_000 });
td.setInstrumentPositionLimit(SYMBOL, { long: 10, short: 10 });
td.trackMarketData(md);              // 偏离与金额闸共用的新鲜参考价

// 2) 绝不让一个有 bug 的处理函数拖垮数据面——记录处理函数抛出的异常。
md.on("error", (e) => console.error("[md handler]", e));
td.on("error", (e) => console.error("[td handler]", e));

// 3) 连接时及每次重连时握手（CTP 会自行重连）。
td.on("front-connected", async () => {
  try { await td.session({ ...CREDS }); console.log("交易就绪"); }
  catch (e: any) { console.error("session 失败", e.errorId, e.errorMsg); }
});
md.on("front-connected", async () => {
  await md.login();                  // SimNow 行情前置接受匿名登录
  md.subscribe([SYMBOL]);
});

// 4) 持仓由「成交」推导，绝不凭发送臆测。
let position = 0; // 净手数（+多 / -空）
td.on("rtn-trade", (t: Trade) => {
  position += t.direction === Direction.Buy ? t.volume : -t.volume;
  console.log("成交", t.price, "x", t.volume, "-> 净持仓", position);
});

// 5) 策略：把每个 tick 转成目标，并交易差额。
md.on("rtn-depth-market-data", async (tick: DepthMarketData) => {
  if (tick.instrumentId !== SYMBOL) return;
  const want = signal(tick);         // 你的逻辑：目标净手数，如 -1 / 0 / +1
  const delta = want - position;
  if (delta === 0) return;
  const buy = delta > 0;
  await td.reqOrderInsert({
    instrumentId: SYMBOL,
    direction: buy ? Direction.Buy : Direction.Sell,
    // 在扩大持仓时开仓，在缩小持仓时平仓
    combOffsetFlag: Math.sign(want) === Math.sign(position) || position === 0 ? OffsetFlag.Open : OffsetFlag.CloseToday,
    limitPrice: buy ? tick.askPrice1 : tick.bidPrice1, // 越过价差以求成交
    volumeTotalOriginal: Math.abs(delta),
  }).catch((e) => console.warn("报单被拒:", e.message));
});

function signal(_tick: DepthMarketData): number { return 0; /* TODO 你的 alpha */ }

// 6) 优雅关闭——先熔断，再 close 一次。
for (const sig of ["SIGINT", "SIGTERM"] as const) {
  process.on(sig, () => { td.haltAll(); td.close(); md.close(); process.exit(0); });
}
```

### 健壮的重连处理

CTP 会自行重连；你在每次 `front-connected` **原地**重新跑握手——绝不要用 `new Trader()` 来重连
（厂商 DLL 会死锁；见 [native-hooks.zh-CN.md](native-hooks.zh-CN.md#进程生命周期-一次创建并复用)）。

```ts
td.on("front-connected", async () => {
  // session() 会重新认证、重新登录、重新确认，并且关键地重新同步乘数/持仓/报单——使风控引擎的
  // 持仓上限重建到包含断线前就在挂着的报单（以及同账户另一终端下的）。然后重新预埋你需要的触发器。
  await td.session({ ...CREDS });
  rearmTriggers();
});

td.on("front-disconnected", (reason) => {
  console.warn("交易链路断开（码", reason, "）—— CTP 会自动重连");
  // 在途请求 Promise 已经 reject；若你的策略需要已确认的会话，
  // 就暂停发新信号，直到下一次 front-connected。
});

function rearmTriggers() { /* 在这里重新创建任何 td.arm(...) 触发器 */ }
```

如果你倾向于手写而非用 `session()`，关键是**重连后重新同步报单**，以便重建在途预约：

```ts
td.on("front-connected", async () => {
  await td.reqUserLogin({ ...CREDS });
  await td.syncMultipliers();   // 在 syncOrders 之前，使预约成本用对乘数
  await td.syncOrders();        // 重建预约，同时把持仓侧标为不确定
  await td.syncPositions();     // 最后提交已持仓快照并保留预约
});
```

### 组合与多腿下单

**平今 vs 平昨。** SHFE/INE 区分两者，所以光用 `"平仓"` 不够——要选 `CloseToday`（`"3"`）或
`CloseYesterday`（`"4"`）。要平掉 5 手多头（其中 2 手是今仓、3 手是历史仓），发两笔：

```ts
import { Direction, OffsetFlag } from "@hitrading/ctp-node";

await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseToday,     limitPrice: px, volumeTotalOriginal: 2 });
await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseYesterday, limitPrice: px, volumeTotalOriginal: 3 });
```

**多腿开平标志明确不支持。** 本地账本是单合约、单方向模型，无法证明 `"00"`、`"30"`、`"03"`
等值的逐腿持仓与保证金正确性。因此 `reqOrderInsert()` 和 `arm()` 都要求恰好一个开平字符，多腿模板会
在到达 CTP 前被拒绝。需要组合合约时，应在具备逐腿账本的更高层网关中实现。

实时回报与 `syncOrders()` 也会识别由其他终端提交的不支持工作单；只要仍有此类订单，所有开仓
请求就会 fail-closed，而不会猜测逐腿风险。

**建立/拆解组合持仓**（在交易所支持时）走 `reqCombActionInsert`，用 `InputCombAction`：

```ts
await td.reqCombActionInsert({
  brokerId: "9999", investorId: "your-id",
  instrumentId: "SPC m2509&m2601",  // 组合合约
  combDirection: "0",               // 0 = 建立组合，1 = 拆分
  volume: 1,
  // ……其余 InputCombAction 字段按你的交易所
});
```

> `reqCombActionInsert` 只要配置本地风控或模式不是 `NORMAL` 也会被拒绝；它不在本 binding 的
> 定量风控 allowlist 内。
