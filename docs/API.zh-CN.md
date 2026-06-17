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
- [类型](#类型) — `RiskConfig`、`LotCap`、`SessionOptions`、`ArmSpec`、`ArmHandle`、`CallbackOptions`、`RspInfo`、`MdLoginReq`
- [枚举与结构体类型](#枚举与结构体类型)
- [风控一览](#风控一览)
- [实战示例](#实战示例) — 策略骨架 · 重连处理 · 组合下单

```ts
import {
  MarketData, Trader,
  Direction, OffsetFlag, OrderPriceType, // 生成的枚举
  type RiskConfig, type SessionOptions, type DepthMarketData,
} from "@hitrading/ctp-node";
```

---

## 安装

```bash
npm install @hitrading/ctp-node
```

win32-x64、linux-x64、darwin-x64 提供预编译二进制；其他平台在安装时从源码编译（与任何
node-gyp 包一样，需要 C++ 工具链 + Python）。

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

td.riskSet({ maxOrderVolume: 10, maxNotional: 5_000_000, maxOrdersPerSec: 20 });
td.setMaxPosition("rb2510", 100); // 单边持仓不超过 100 手

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

**一次创建，进程内复用。** `MarketData` / `Trader` 只构造一次并一直持有。CTP 会自行重连——断线
后应**原地**处理：在 `front-connected` 再次触发时（首次连接 *以及* 每次自动重连都会触发）重新
跑一遍握手。**不要**销毁再重建客户端：厂商 DLL 在一个进程内做几次 create/close 循环后会在
`Release()` 内部死锁。关闭时调用一次 `close()`（或干脆不调用——进程退出会清理）。详见
[native-hooks.zh-CN.md → 进程生命周期](native-hooks.zh-CN.md#进程生命周期-一次创建并复用)。

**背压。** 每个客户端从无锁环形缓冲解码行情/回报。`droppedRecords` 统计 JS 循环跟不上时丢弃的
记录数。行情丢弃**最旧**的记录（你总能看到最新报价）；交易端丢弃**最新**的，因此报单/成交回报
永远不会被静默丢弃。

**盘前风控在 C++ 中执行。** 所有风控限制（`riskSet`、`setMaxPosition`、`halt` 等）都在原生
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

### `md.login(req?)` → `Promise<RspUserLogin>`

登录行情前置。成功时以登录响应 resolve，失败时以 CTP 错误 reject（`err.errorId`、`err.errorMsg`）。

| `req` 字段（`MdLoginReq`，均可选） | 类型 | 含义 |
|---|---|---|
| `brokerId` | `string` | 经纪商代码。 |
| `userId` | `string` | 投资者/用户代码。 |
| `password` | `string` | 密码。 |
| `tradingDay` | `string` | 交易日（一般无需）。 |
| `userProductInfo` | `string` | 产品信息标识。 |

> SimNow 行情前置接受匿名 `login({})`；实盘前置需要凭证。

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

### `md.logout(req?)` → `Promise<UserLogout>`

登出。`req` 可带 `{ brokerId?, userId? }`。

```ts
await md.logout();
// 或指定账户
await md.logout({ brokerId: "9999", userId: "id" });
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

某合约最新的完整深度 tick，**同步**地从 C++ 末值缓存读取。该缓存在**每个** tick 抵达 JS 之前就
已更新，因此无需等待事件、也无需在策略里做逐 tick 记账。若该合约还没有任何 tick（或其缓存项已被
清除）则返回 `null`。缓存项在 `unsubscribe` 时清除，全部缓存项在 `close` 时清除。

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

某合约的最新价，**同步**地从同一 C++ 缓存读取。若还没有任何 tick（或缓存项已被清除——见
[`snapshot`](#mdsnapshotinstrumentid--depthmarketdata--null)）则返回 `0`。

```ts
const px = md.last("rb2510");
if (px > 0) console.log("最新成交价", px);
```

### `md.attachArm(trader)` → `void`

把这路行情的 tick 路由到某个 `Trader` 的预埋触发器（见
[`Trader.arm`](#tdarmmd-spec--armhandle)）。通常你调用 `td.arm(md, …)`，它会替你调用本方法。
一路 `MarketData` 只服务于一个 `Trader` 的触发器；再 attach 另一个不同的 `Trader` 会抛错。

```ts
// 显式调用（一般不需要——td.arm() 会自动做）：
md.attachArm(td);

// attach 第二个不同的 trader 会抛错：
md.attachArm(td);        // ok
md.attachArm(otherTd);   // 抛错：已在服务另一个 Trader
```

### `md.getApiVersion()` → `string` / `md.getTradingDay()` → `string`

CTP API 版本字符串，以及当前交易日（`YYYYMMDD`），连接后可用。

```ts
console.log("CTP 行情 API", md.getApiVersion()); // 如 "6.7.2"
md.on("front-connected", () => console.log("交易日", md.getTradingDay()));
```

### `md.droppedRecords` → `number`（getter）

背压下丢弃的 tick 总数（丢最旧）。监控它以发现消费慢的情况。

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

### `td.session(opts)` → `Promise<{ multipliers, positions, orders }>`

连接后的一站式握手：**认证 → 登录 → 确认结算单 → 同步乘数 / 持仓 / 报单**。在 `front-connected`
处理函数里调用（重连后再调一次）。返回各同步步骤的行数。每一步只是包装了对应的请求/同步方法，
所以需要更细控制时你也可以自己手写流程。

`opts` 是 [`SessionOptions`](#sessionoptions)：

| 字段 | 类型 | 含义 |
|---|---|---|
| `brokerId` | `string` | 经纪商代码（必填）。 |
| `userId` | `string` | 投资者代码（必填）。 |
| `password` | `string` | 密码（必填）。 |
| `appId` | `string?` | 终端认证的 app id。SimNow：`"simnow_client_test"`。 |
| `authCode` | `string?` | 终端授权码。SimNow：16 个零。两者都省略则跳过认证。 |
| `confirmSettlement` | `boolean?` | 登录后确认结算单（实盘账户必须确认，否则报单被拒）。默认 `true`。 |
| `sync` | `object?` | 登录后拉取哪些风控输入。`{ multipliers?: boolean \| string[], positions?: boolean, orders?: boolean }`；默认全部拉取。`multipliers: true` 查询全部合约；传符号列表可限定范围。 |

```ts
// SimNow 完整握手
td.on("front-connected", async () => {
  const counts = await td.session({
    brokerId: "9999", userId: "id", password: "pw",
    appId: "simnow_client_test", authCode: "0000000000000000",
  });
  console.log("已同步", counts); // { multipliers, positions, orders }
});

// 把乘数同步限定到你交易的合约（冷启动更快）
await td.session({
  brokerId: "9999", userId: "id", password: "pw",
  sync: { multipliers: ["rb2510", "au2508"], positions: true, orders: true },
});

// 跳过结算单确认（不需要的环境）
await td.session({ brokerId: "9999", userId: "id", password: "pw", confirmSettlement: false });
```

> 手写等价流程：`await td.reqAuthenticate(...)` → `await td.reqUserLogin(...)` →
> `await td.reqSettlementInfoConfirm(...)` → `await td.syncMultipliers()` →
> `await td.syncPositions()` → `await td.syncOrders()`。

### `td.reqOrderInsert(req?)` → `Promise<void>`

通过 C++ 盘前风控网关报单。**提交成功即 resolve**——CTP 对已接受的报单不返回成功应答，只有
`rtn-order` / `rtn-trade`（用 `orderRef` 关联）。只有发送本身被拒（风控网关、限频、或 CTP API
错误码）才会 **reject**。`orderRef` 留空会自动分配一个唯一的数字 ref。

`req` 是 `Partial<InputOrder>`；你几乎总会设置的字段：

| 字段 | 类型 | 含义 |
|---|---|---|
| `instrumentId` | `string` | 合约，如 `"rb2510"`。 |
| `direction` | `Direction`（`"0"`/`"1"`） | `Buy` / `Sell`。 |
| `combOffsetFlag` | `string` | 开平：`"0"` 开仓、`"1"` 平仓、`"3"` 平今……（每条腿一个字符）。 |
| `limitPrice` | `number` | 限价；市价/任意价单填 `0`（并相应设置 `orderPriceType`）。 |
| `volumeTotalOriginal` | `number` | 数量（手）。 |
| `orderRef` | `string?` | 留空则自动分配唯一 ref；设置它可关联产生的 `rtn-order`。 |
| `orderPriceType` | `OrderPriceType?` | 默认限价；市价/最优价类型需设置。 |

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
await td.reqOrderInsert({ orderRef: "my-42", instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 2 });
td.on("rtn-trade", (t) => { if (t.orderRef === "my-42") console.log("我的报单成交", t.volume); });

// 4）平今一笔空头（卖 -> 平）
await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseToday, limitPrice: 3490, volumeTotalOriginal: 1 });
```

### `td.reqOrderAction(req?)` → `Promise<void>`（撤单）

撤/改一笔挂单。提交成功即 resolve。用 `orderRef` + `frontId` + `sessionId`（取自其 `rtn-order`）
或用 `exchangeId` + `orderSysId` 来定位报单。

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

### 查询方法 — `td.reqQry*(req?)` → `Promise<unknown[]>`

每个 `reqQry*` 以**行数组** resolve（多行响应会累积；空结果 resolve `[]`）。CTP 对查询限频约每秒
一次——[`sync*`](#tdsyncmultipliersinstrumentids--promisenumber) 辅助方法已替你处理重试/退避；
原始 `reqQry*` 请谨慎调用。

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
合约；传符号列表可限定范围。会穿越冷启动限频重试。返回带有乘数的合约数量。

```ts
const n = await td.syncMultipliers();           // 全部合约
await td.syncMultipliers(["rb2510", "au2508"]);  // 仅这些
console.log(n, "个合约带有乘数");
```

### `td.syncPositions(opts?)` → `Promise<number>`

从 CTP（`reqQryInvestorPosition`，取每行的 `UseMargin`）拉取持仓**保证金**并播种到风控引擎的持仓
跟踪器。除非传入 `{ brokerId?, investorId? }`，否则用已登录账户。返回播种的持仓数。

```ts
const held = await td.syncPositions();
console.log("已播种", held, "个持仓，保证金 =", td.positionCost());
// 显式账户
await td.syncPositions({ brokerId: "9999", investorId: "id" });
```

### `td.syncOrders(opts?)` → `Promise<number>`

从 CTP 当前挂单（`reqQryOrder`）重建在途预约——一次权威重同步。登录后和任何重连后都应调用，
使持仓上限把已在经纪商挂着的报单也算进去（重连前下的，或——因为 CTP 也会投递——同账户另一终端
下的）。先跑 `syncMultipliers()`，预约成本才会用对乘数。返回重新预约的挂单开仓单数。

```ts
await td.syncMultipliers();
await td.syncPositions();
const working = await td.syncOrders();
console.log("重新预约了", working, "笔挂单");
```

### 风控配置

#### `td.riskSet(config)` → `this`

把盘前风控限制发布给 C++ 执行器（立即生效）。非有限值（`NaN`/`Infinity`）会被**抛错拒绝**，而不
是静默关闭该控制。`config` 是 [`RiskConfig`](#riskconfig)；省略某字段或传 `0`/负数即关闭该控制。

```ts
// 全套
td.riskSet({
  maxOrderVolume: 10,          // 单笔 ≤ 10 手
  maxPriceDeviation: 0.02,     // 距参考价 ≤ 2%
  maxNotional: 5_000_000,      // 单笔名义价值 ≤ 5M
  maxOrdersPerSec: 20,         // 令牌桶限频
  orderBurst: 40,              // 桶容量（默认 = maxOrdersPerSec）
  maxMargin: 20_000_000,       // 全账户开仓保证金 ≤ 20M
});

// 只设几项（其余保持关闭）
td.riskSet({ maxOrderVolume: 5, maxOrdersPerSec: 10 });

// 传 0 关闭某控制
td.riskSet({ maxNotional: 0 });
```

#### `td.halt()` / `td.resume()` → `this`

一键熔断。`halt()` 立即拦截所有会立刻抵达交易所的开仓类发送（普通、执行宣告、报价、询价、期权
自对冲、组合动作等 insert）。撤单和其他 *action* 故意保持开放，使你在熔断时仍能撤回挂单；预埋/
暂存单（不会立即发往交易所）同样不被拦截。`resume()` 解除。

```ts
// 应急信号下停止所有新报单，但保留撤单能力
process.on("SIGINT", () => td.halt());

// 评估情况后重新启用交易
td.resume();

// 例：PnL 突破限额则熔断
td.on("rtn-trade", () => { if (computePnl() < -100000) td.halt(); });
```

#### `td.setMaxPosition(instrumentId, max)` / `td.setMaxPositions(limits)` → `this`

按合约限制开仓持仓（手数），在每笔开仓单上执行。`max` 是 [`LotCap`](#lotcap)：一个数字同时限制
两边；`{ long, short }` 分别限制各边。在 `{ long, short }` 内，**省略某一边会让该边的当前上限保持
不变**（传 `0`/负数清除）。多空独立跟踪。校验按 committed = 已持仓 + 在途（挂单）量。

```ts
td.setMaxPosition("rb2510", 100);                  // 多 ≤ 100 且空 ≤ 100
td.setMaxPosition("au2508", { long: 50, short: 10 }); // 不对称

// 只抬高多头上限，空头保持原样
td.setMaxPosition("au2508", { long: 80 });
// 清除空头上限
td.setMaxPosition("au2508", { short: 0 });

// 批量
td.setMaxPositions({ rb2510: 100, ru2510: { long: 100, short: 20 }, au2508: 10 });
```

#### `td.setMaxPositionCost(instrumentId, maxCost)` / `td.setMaxPositionCosts(limits)` → `this`

限制单个合约的开仓持仓**保证金**（Σ 价 × 量 × 乘数 × 保证金率，多空相加——一个按合约的资金/
集中度限制）。按合约计，与账户级 `riskSet({ maxMargin })` 相互独立；两者同时生效。传
`maxCost ≤ 0` 移除。在某合约的保证金率已知之前（由 CTP 自动喂入——查询
`reqQryInstrumentMarginRate`），它按全名义价值计（保守）。

```ts
td.setMaxPositionCost("au2508", 5_000_000); // 黄金保证金 ≤ 5M
td.setMaxPositionCosts({ ag2508: 2_000_000, au2508: 5_000_000 });
td.setMaxPositionCost("au2508", 0);         // 移除上限
```

#### `td.trackMarketData(md)` → `this`

提供 `maxPriceDeviation` 校验所对照的实时参考价。把这路 `MarketData` 的 C++ 快照缓存直接接入本
Trader 的风控引擎，使偏离参考价**在 C++、于报单发送路径上**读取——无 JS 往返，且覆盖在 C++ 里
触发、JS 不在回路中的预埋单。**不调用它就没有参考价，偏离校验会被跳过。**

```ts
td.riskSet({ maxPriceDeviation: 0.02 });

// 从行情实时取参考价（每笔报单都在 C++ 读取，含预埋单）
td.trackMarketData(md);
```

> 合约乘数（用于名义价值 / 成本计算）和按合约的保证金率（用于 `maxMargin` 上限）**不**从 JS
> 喂入：C++ 风控引擎直接从 CTP 的 `OnRspQryInstrument` / `OnRspQryInstrumentMarginRate` 回调里
> 取。所以任何 `reqQryInstrument` 都会保持乘数最新（见
> [`syncMultipliers`](#tdsyncmultipliersinstrumentids--promisenumber)），而对你交易的合约查询
> `reqQryInstrumentMarginRate` 会让保证金上限精确——没有可从 JS 调用的 setter，且这些校验同样
> 覆盖预埋单。

### 持仓保证金跟踪

#### `td.seedPosition(instrumentId, side, volume, margin)` → `this`

播种一个已有持仓的真实**保证金**（供 `maxMargin` / `maxPositionCost` 上限用），适用于你自己对账
而非用 `syncPositions()` 的场景。`margin` 是实际占用的资金——CTP `InvestorPosition.UseMargin`——
即保证金上限所跟踪的单位。

| 参数 | 类型 | 含义 |
|---|---|---|
| `instrumentId` | `string` | 合约。 |
| `side` | `"long" \| "short"` | 持仓方向。 |
| `volume` | `number` | 持仓手数。 |
| `margin` | `number` | 该持仓占用的真实保证金（`InvestorPosition.UseMargin`）。 |

```ts
// 你已持有 rb2510 多头 3 手，占用保证金 30,000
td.seedPosition("rb2510", "long", 3, 30_000);
// 以及 au2508 空头 2 手，占用保证金 112,000
td.seedPosition("au2508", "short", 2, 112_000);
```

#### `td.seedFromPositions(positions)` → `this`

从 `reqQryInvestorPosition` 的行播种真实开仓**保证金**（取每行的 `UseMargin`），适用于**净持仓为
分边模式（gross）**的账户（`posiDirection` `"2"` = 多，`"3"` = 空）。保证金上限对两边求和，故分桶
对它们无影响；对净持仓（net）模式合约下的按边*手数*上限，请用 `seedPosition()` 按你想要的方向
播种。

```ts
const rows = await td.reqQryInvestorPosition({ brokerId: "9999", investorId: "id" });
td.resetPositions();
td.seedFromPositions(rows); // （这正是 syncPositions() 所做的）
```

#### `td.positionCost()` → `number`

风控引擎当前跟踪的开仓持仓总保证金（`maxMargin` / `maxPositionCost` 上限所衡量的单位）。

```ts
console.log("账户保证金", td.positionCost());
td.on("rtn-trade", () => console.log("当前保证金", td.positionCost()));
```

#### `td.resetPositions()` → `this`

清空所有跟踪的持仓保证金。（`syncPositions()` 在重新播种前会调用它。）

```ts
td.resetPositions();              // 清空跟踪器
td.seedFromPositions(freshRows);  // 用新一次查询重新播种
```

### 延迟敏感的预埋触发器

#### `td.arm(md, spec)` → `ArmHandle`

预埋一个完全在 C++、于行情回调线程上判定并触发的触发器——当 `md` 看到条件时，报单通过本 Trader
的风控网关发出，**无 JS 往返**。一次性。回执通过普通的 `rtn-order` / `rsp-order-insert` 事件到达
（用 `orderRef` 关联）。

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

#### `td.armStats()` → `{ fired: number; blocked: number }`

预埋触发器的可观测性（它们在 C++ 里触发，JS 不在回路中）：有多少触发并发出了（`fired`）vs 被
风控网关/发送拒绝了（`blocked`）。在你预期会触发的触发之后轮询它——被拦截的预埋单否则不可见。

```ts
const { fired, blocked } = td.armStats();
console.log(`预埋：${fired} 已发，${blocked} 被拒`);
if (blocked > 0) console.warn("有一笔预埋单被风控拒绝");
```

### `td.getApiVersion()` / `td.getTradingDay()` → `string`

CTP 交易 API 版本与当前交易日。

```ts
console.log("CTP 交易 API", td.getApiVersion(), "交易日", td.getTradingDay());
```

### `td.close()` / `td.droppedRecords`

同 [`CtpClient`](#ctpclient)。交易端的环是丢**最新**（可靠），所以背压下报单/成交回报永不被静默
丢弃。

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

自构造起背压下丢弃的记录总数。持续攀升说明消费跟不上。行情丢最旧；交易端丢最新。

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

- **`reqQry*`**（查询）→ `Promise<unknown[]>`（行数组；空则 `[]`）。
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
`reqOrderInsert`、`reqOrderAction`。（完整集合由 CTP 头文件生成——共 111 个请求方法。）

---

## 类型

### `RiskConfig`

[`riskSet`](#tdrisksetconfig--this) 的盘前风控限制。所有字段可选；**省略或 `0` = 关闭**；
`NaN`/`Infinity` 抛错。

| 字段 | 类型 | 含义 |
|---|---|---|
| `maxOrderVolume` | `number` | 单笔最大手数。 |
| `maxPriceDeviation` | `number` | 最大 `\|价 − 参考价\| / 参考价` 比例（如 `0.02` = 2%）；需经 [`trackMarketData`](#tdtrackmarketdatamd--this) 提供参考价。 |
| `maxNotional` | `number` | 单笔最大名义价值（价 × 量 × 乘数）。 |
| `maxOrdersPerSec` | `number` | 每秒最大报单发送数（令牌桶）。 |
| `orderBurst` | `number` | 令牌桶容量。默认：`maxOrdersPerSec`。 |
| `maxMargin` | `number` | 全账户开仓持仓总**保证金**上限 = Σ(价 × 量 × 乘数 × 保证金率)——实际占用的资金。在每笔开仓单（含预埋单）上于 C++ 执行。保证金率由 CTP 自动喂入（查询 `reqQryInstrumentMarginRate` 以填充；在某合约的保证金率已知之前，按全名义价值计 = 保守）。 |
| `maxPositionCost` | `number` | `maxMargin` 的**已弃用**别名（现也基于保证金）。两者都设时以 `maxMargin` 为准。 |

```ts
const conservative: RiskConfig = { maxOrderVolume: 5, maxOrdersPerSec: 10, maxNotional: 2_000_000 };
td.riskSet(conservative);
```

### `LotCap`

`number | { long?: number; short?: number }` ——
[`setMaxPosition`](#tdsetmaxpositioninstrumentid-max--tdsetmaxpositionslimits--this) 的上限参数。
一个数字同时限制两边；`{ long, short }` 分别限制（省略某边 = 不变，`≤ 0` = 清除）。

```ts
const a: LotCap = 100;                    // 两边都 ≤ 100
const b: LotCap = { long: 50, short: 5 }; // 多 ≤ 50，空 ≤ 5
```

### `SessionOptions`

[`session`](#tdsessionopts--promise-multipliers-positions-orders-) 的选项。字段见那里的表格。

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
| 一键熔断 | `halt()` / `resume()` | 所有开仓发送 | `resume()` |
| 单笔最大手数 | `riskSet({ maxOrderVolume })` | 每笔 | `0` / 省略 |
| 最大价格偏离 | `riskSet({ maxPriceDeviation })` + 参考价 | 每笔 | `0` / 无参考价 |
| 最大名义价值 | `riskSet({ maxNotional })` | 每笔 | `0` / 省略 |
| 限频 | `riskSet({ maxOrdersPerSec, orderBurst })` | 每秒 | `0` / 省略 |
| 最大持仓手数 | `setMaxPosition(id, …)` | 每合约、每边 | `≤ 0` |
| 最大持仓保证金（按合约） | `setMaxPositionCost(id, …)` | 每合约 | `≤ 0` |
| 最大持仓保证金（全账户） | `riskSet({ maxMargin })` | 全账户 | `0` / 省略 |

持仓手数和持仓保证金上限都按 **committed = 已持仓（已成）+ 在途（挂单）量** 计算，因此一连串
开仓不会在成交回报到达前溜过上限。用 `syncPositions()` / `seedPosition()` 喂入已持仓，用
`syncOrders()` 喂入挂单（尤其是重连后）。保证金率由 CTP 自动喂入（查询
`reqQryInstrumentMarginRate`）；在某合约的保证金率已知之前，按全名义价值计（保守）。所有上限都
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
td.riskSet({ maxOrderVolume: 5, maxNotional: 2_000_000, maxOrdersPerSec: 10, maxPriceDeviation: 0.02, maxMargin: 5_000_000 });
td.setMaxPosition(SYMBOL, { long: 10, short: 10 });
td.trackMarketData(md);              // 偏离校验用的实时参考价

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
  process.on(sig, () => { td.halt(); td.close(); md.close(); process.exit(0); });
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
  await td.syncPositions();     // 重建已持仓成本
  await td.syncOrders();        // 从挂单重建在途预约
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

**价差 / 组合合约（多腿 `combOffsetFlag`）。** `combOffsetFlag` 是**每条腿一个开平字符**。对于
交易所挂牌的组合合约（如 SHFE 跨期价差 `SP rb2510&rb2601`，两条腿），传一个 2 字符的 flag——这里
两条腿都开仓（`"0"` + `"0"` → `"00"`）：

```ts
await td.reqOrderInsert({
  instrumentId: "SP rb2510&rb2601", // 交易所专有的组合 id 格式
  direction: Direction.Buy,
  combOffsetFlag: "00",             // 腿1 开、腿2 开
  limitPrice: spreadPrice,
  volumeTotalOriginal: 1,
});
// 平今近腿、开远腿的两腿组合："30"
```

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

> 多腿 flag 和组合合约都是**交易所专有**——id 格式以及存在哪些组合因交易所而异。盘前风控网关的
> 按合约上限以组合合约 id（而非底层各腿）为键。
