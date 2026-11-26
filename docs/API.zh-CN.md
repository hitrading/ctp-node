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
- [类型](#类型) — `RiskLimits`、`SessionOptions`、`CallbackOptions`、`RspInfo`、`MdLoginReq`
- [枚举与结构体类型](#枚举与结构体类型)
- [风控一览](#风控一览)
- [实战示例](#实战示例) — 策略骨架 · 重连处理 · 组合下单

从 4.x 升级？已删除的账本、上限与预授权单（arm，本地行情触发单）见 [MIGRATION.md](MIGRATION.md)。
CTP 自身的预埋单接口（`reqParkedOrderInsert` 等）仍保留，按不支持的请求处理（见[风控配置](#风控配置)）。

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
C++ 工具链与 Python（macOS 上为 Xcode Command Line Tools）。随包的 CTP macOS 框架是 6.7.13 通用版，
与 Windows/Linux 库是同一 CTP 版本。

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

td.setRiskLimits({ maxOrderVolume: 10, maxOrdersPerSec: 20 }); // C++ 逐笔闸门
// 账户级上限（保证金、持仓手数）放在你自己的交易层。

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

**盘前风控在 C++ 中执行，而且是无状态的。** 风控限制（`setRiskLimits`）与风控模式（`reduceOnly` /
`haltAll`）都在原生插件内、报单发送路径上、抵达 CTP 之前就执行——没有 JS 往返。被拦截的报单会让
`reqOrderInsert()` reject，原因在错误消息里。每条规则只依据这一笔报单本身、最新行情参考价
（`trackMarketData`）和静态合约元数据（来自 `reqQryInstrument`）判断。本绑定不维护持仓或挂单账本，
所以重连后没有要重建的东西，也不会与你自己的账本不一致。账户级敞口上限——账户保证金、单合约保证金、
持仓手数——属于掌握账户事实的交易层，见[账户事实与账户级上限](#账户事实与账户级上限)。

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
> `requestId` 固定为 0，因此 binding 会主动拒绝重叠的会话切换。若一次请求超时，原生行情缓存
> 会保持 fail-closed：先忽略第一条迟到的最终回包（或等待前置状态变化重置），之后才允许
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

交易客户端：报单、查询，以及无状态的原生盘前风控闸门。继承
[`CtpClient`](#ctpclient)；所有生成的 `reqXxx` 请求方法都可用（见
[请求方法](#生成的请求方法)）。

### `new Trader(flowPath, fronts)`

参数同 [`MarketData`](#new-marketdataflowpath-fronts)。Trader 会记住已确认的账户，因此
`sync*` 辅助方法从不需要传账户字段；并会把自动 `OrderRef` 计数器播种到经纪商 `maxOrderRef` 之上，
使 ref 永不与上一会话冲突。

```ts
const td = new Trader("./flow/td/", "tcp://182.254.243.31:30002");
// 故障切换前置：
const td2 = new Trader("./flow/td/", ["tcp://182.254.243.31:30002", "tcp://182.254.243.31:30001"]);
```

### `td.session(opts)` → `Promise<{ identity, multipliers, marginRates }>`

连接后的一站式握手：**认证 → 登录 → 确认结算单 → 同步乘数 / 可选保证金率**。在 `front-connected`
处理函数里调用（重连后再调一次）。返回经请求关联确认的
`identity: { frontId, sessionId, maxOrderRef }` 以及各同步步骤的行数。其中 `maxOrderRef`
是本次登录响应报告的值；Trader 的自动分配器可能保留更高的进程内高水位。

`session()` 也是把原生报单闸门绑定到交易账户的那一步。握手一开始就会撤销上一会话的报单权限；
只有本次握手确认了新登录的 `frontId` / `sessionId` 和交易账户（`investorId`）之后，普通报单才会
重新被接受。登录本身只是协议步骤：账户要等结算单确认成功之后（或 `confirmSettlement: false` 时
紧接登录之后）、且期间连接没有换代，才在原生与 JS 两侧同时确认。确认之前不接受任何绑定账户的发送；
确认之后的步骤（乘数或保证金率同步）失败时，确认会被重新撤销。新建的 Trader 处于未确认状态，每次前置连接/断开以及每次登录/登出请求都会撤销确认，
所以每次 `front-connected` 都要跑 `session()`。它不查询持仓或挂单：这些放在你自己的账本里（见
[账户事实与账户级上限](#账户事实与账户级上限)）。

`opts` 是 [`SessionOptions`](#sessionoptions)：

登录回报必须包含明确的 `frontId` / `sessionId`，且 `brokerId` / `userId`
必须与请求一致。即使柜台把空回报或身份不匹配的回报标记为成功，
`session()` 也会拒绝继续：这些字段标识了原生报单闸门所绑定的会话，接受旧/零会话或其他账户
会让报单以过期身份发出。

| 字段 | 类型 | 含义 |
|---|---|---|
| `brokerId` | `string` | 经纪商代码（必填）。 |
| `userId` | `string` | 投资者代码（必填）。 |
| `investorId` | `string?` | 为报单确认的交易账户。默认等于 `userId`；登录操作员与投资者不同时显式设置。 |
| `password` | `string` | 密码（必填）。 |
| `appId` | `string?` | 终端认证的 app id。SimNow：`"simnow_client_test"`。 |
| `authCode` | `string?` | 终端授权码。SimNow：16 个零。两者都省略则跳过认证。 |
| `confirmSettlement` | `boolean?` | 登录后确认结算单（实盘账户必须确认，否则报单被拒）。每一条带经纪商或投资者字段的回执行都必须是本账户，否则握手失败；合法的空末包可以接受。默认 `true`。 |
| `sync` | `object?` | `{ multipliers?: boolean \| string[], marginRates?: { instrumentIds: string[], hedgeFlag: "1" \| "2" \| "3" \| "5" \| "6" \| "7" } }`。乘数默认拉取（全部合约；传列表限定范围，传 `false` 跳过）；保证金表必须显式给出有限合约列表和套保标志，因为 CTP 每个合约需一次查询，且不同套保标志使用不同费率表。 |

```ts
// SimNow 完整握手
td.on("front-connected", async () => {
  const counts = await td.session({
    brokerId: "9999", userId: "id", password: "pw",
    appId: "simnow_client_test", authCode: "0000000000000000",
  });
  console.log("已同步", counts); // { identity, multipliers, marginRates }
});

// 把乘数同步限定到你交易的合约（冷启动更快）
await td.session({
  brokerId: "9999", userId: "id", password: "pw",
  sync: {
    multipliers: ["rb2510", "au2508"],
    marginRates: { instrumentIds: ["rb2510", "au2508"], hedgeFlag: "2" },
  },
});

// 跳过结算单确认（不需要的环境）
await td.session({ brokerId: "9999", userId: "id", password: "pw", confirmSettlement: false });
```

> 每一步都包装了对应的请求/同步方法：`reqAuthenticate(...)` → `reqUserLogin(...)` →
> `reqSettlementInfoConfirm(...)` → `syncMultipliers()` → `syncMarginRates([...])`。手写流程可以发
> 原始查询和 `syncMultipliers()`，但裸 `reqUserLogin()` 不会发布报单账户：只有 `session()` 会在
> 原生与 JS 两侧确认交易账户。手写登录之后 `reqOrderInsert()` 与 `syncMarginRates()` 会被拒绝
> （`a confirmed Trader login is required` / `syncMarginRates requires a successful trader login first`），
> 其余绑定账户的发送由原生闸门拒绝（`session account not confirmed`）。

### `td.reqOrderInsert(req?)` → `Promise<void>`

通过 C++ 盘前风控网关报单。**提交成功即 resolve**——CTP 对已接受的报单不返回成功应答，只有
`rtn-order` / `rtn-trade`（用 `orderRef` 关联）。只有发送本身被拒（风控网关、限频、或 CTP API
错误码）才会 **reject**。`orderRef` 留空会自动分配一个唯一的数字 ref。

普通报单要求已完成 `session()` 确认，`brokerId` 和 `investorId` 会自动从当前会话补齐。
显式传入不一致的值会在编码前被拒绝，因此报单只能以原生闸门已确认的账户发出。

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

> **只接受单合约的 GFD、FAK 和 FOK 限价单。** GFD 使用 `timeCondition: "3", volumeCondition: "1"`，FAK 使用 `"1", "1"`，FOK 使用 `"1", "3"`。`reqOrderInsert` 会主动拒绝（抛 `TypeError`）原生闸门无法按“一个合约、一个限价”判断的其它时间/成交量条件组合、最小成交量、触发、止损、互换、强平等变体，以及多腿的 offset/hedge 标志。

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
间隔按登录后前置下发的流控（见 `td.frontInfo`）计算：不超过 `qryFreq`、`ftdPkgFreq` 的一半和每秒 10 笔，
尚未取得流控时至少间隔 1100 ms；并发直接调用也无法绕过限频，因此不要再加 `setTimeout(1000)`。
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

触发合约查询，使 C++ 风控引擎取到合约乘数与产品类别——它直接从 CTP 的 `OnRspQryInstrument` 响应里取，
因此本方法不调用任何 JS setter。按乘数精确执行 `maxNotional` 校验所必需。不传参则一次请求查询全部
合约；传符号列表可限定范围。显式空数组是返回 `0` 的 no-op，只有省略参数才表示“查询全部”。会穿越
冷启动限频重试，返回所应答合约中原生引擎实际发布了可用乘数的数量，因此计数遵守下面的全部规则（6.0.2 起）。

合约刷新会在请求发出前先使所查合约（不带任何过滤条件时为全部合约）的旧乘数失效，因此失败/空刷新不会
继续使用陈旧元数据；在刷新成功前，已启用的 `maxNotional` 校验对这些合约 fail-closed。只按交易所、交易所
合约代码或品种过滤的查询（6.0.1 起）不使任何合约失效，成功后合并发布，不会清掉范围外的合约。一次合约查询的
各行先按请求号在原生侧暂存，只有该查询的末包无错误到达时才一起生效。查询过滤条件之外的行不会暂存；同一合约的
多行在交易所、产品类别或乘数上不一致时，该合约不发布乘数；范围外但合约代码相同的行也算作不一致（6.0.2 起）。
这样被判为身份不明的合约，在不带交易所、品种、交易所合约代码过滤的查询（全部合约或该合约代码）成功、可读且带回
该合约一致的行之前，过滤查询不会再发布它；失败、空应答、超时或含不可读行的应答都不会解除这一标记；任何应答里看到的冲突，无论该查询以何种方式结束都会记入。发送被拒、错误末包、该请求的通用 `rsp-error`、空闲超时、任何撤销全部在途请求的情形
（前置连接/断开、`session()` 开始、主动登录/登出）或更新的合约查询都会丢弃暂存行，被取代查询的迟到行也会被忽略；
若撤销发生在末包已发布之后（例如在其他回调里发起登录、而该应答尚未被取走），已发布的不会回滚。有一行合约代码、
交易所、品种或交易所合约代码无法读取时，即便是过滤查询，成功后也会清空全部合约。

原始 `reqQryInstrument()` 的 Promise 只保证这些行是本请求完整且无错误的应答；它不会剔除过滤条件之外的行、
不会处理不一致的重复行、也无法识别不可读字段，这些由原生发布处理。若你用这些行自建合约规格，请按同样规则
处理（剔除过滤条件之外的行；同一合约多行不一致即作废该合约）。

```ts
const n = await td.syncMultipliers();           // 全部合约
await td.syncMultipliers(["rb2510", "au2508"]);  // 仅这些
console.log(n, "个合约带有乘数");
```

### `td.syncMarginRates(instrumentIds, hedgeFlag?)` → `Promise<number>`

为有限交易合约集解析投资者多空按金额/按手保证金率（套保标志默认 `"1"`，投机），供**你自己**计算
保证金——原生闸门不使用它们。投资者表按当次完整合约目录中的 ProductID 分组查询；合约级行优先于
品种行，即使合约级行非法也不继承品种设置。仅当返回 `IsRelative` 时才追加交易所基础表查询，相对
投资者表与该基础表逐字段相加。共享调度器会自动间隔发送每笔查询。返回得到可用费率的合约数量。

6.0 起每一行先核对归属再使用：经纪商必须是本账户、投资单元必须为空；行带交易所代码时须是该合约在
合约目录中的交易所（6.0.1 起，投资者表与交易所基础表都核对）；单一投资者范围的行须是本投资者号，投资者组
范围的行须是本账户的投资者组号（仅在出现经纪商、投资单元与交易所都归属本账户的投资者组行时，才用
`reqQryInvestor` 查询一次），
全部投资者范围的行不核对投资者字段（可能是通配值）。合约目录须给每个所请求合约唯一的身份（同一合约的目录行
在品种或交易所上不一致时整次同步失败，6.0.2 起），并给每个所请求品种唯一且非空的交易所（CTP 合约目录都会给）。
只有以所请求合约或其品种为键的行参与处理：同品种其他月份的行既不会触发投资者组查询，也不会导致同步失败
（6.0.2 起）。同一合约有多个适用范围时取最具体的一种（单一投资者
＞投资者组＞全部）；所适用范围内数值冲突、交易所基础表来自其他经纪商或取不到投资者组时，整次同步失败，
不再静默取最后一行。所请求品种在合约目录中的全部行还必须是同一个交易所（6.0.2 起）。

它读取的目录是一次完整的 `reqQryInstrument({})`，按上文会在该目录完成前使全部原生乘数失效；请在放行报单
之前运行，不要在已启用的 `maxNotional` 校验必须持续通过时运行。所请求合约此前解析的结果在同步一开始就被丢弃，
同品种其他合约的结果在同步进行到该品种时丢弃；只有全部所请求合约都解析成功时才保留新结果，部分成功的刷新一个都不保留；
任何重连、登录/登出或 `session()` 都会丢弃全部已解析结果。

### `td.readMarginRates(instrumentIds, hedgeFlag)` → `CtpResolvedMarginRate[]`

返回当前会话最近一次完整 `syncMarginRates()`（或 `session({ sync: { marginRates } })`）解析结果的副本：
`{ instrumentId, hedgeFlag, longMarginRatioByMoney, longMarginRatioByVolume,
shortMarginRatioByMoney, shortMarginRatioByVolume }`。所请求合约没有完整结果时抛错；重连或登录/登出
之后在重新同步前也会抛错，绝不返回旧值。

```ts
await td.syncMarginRates(["rb2510"], "1");
const [rb] = td.readMarginRates(["rb2510"], "1");
// 按你自己的公式：3500 价位的一手多单；乘数取自你自己核对过的合约目录（这里是 `specs`），不要写常数
const { volumeMultiple } = specs.get("rb2510");
const margin = 3500 * volumeMultiple * rb.longMarginRatioByMoney + rb.longMarginRatioByVolume;
```

### 风控配置

#### `td.setRiskLimits(config)` → `this`

把盘前风控限制发布给 C++ 执行器（立即生效）。非有限值（`NaN`/`Infinity`）会被**抛错拒绝**，而不
是静默关闭该控制。`config` 是 [`RiskLimits`](#risklimits)，会整体替换上一份配置；控制字段省略或传
仅 `0` 表示关闭；负数会被拒绝。`maxReferenceAgeMs` 例外：必须为正数，省略时恢复默认 3000 ms。
未知字段（包括已删除的名称 `maxMargin`、`maxAccountMargin`）会直接报错，不会被静默忽略。

手数、偏离、名义价值限制和开仓限频只作用于**开仓**报单。平仓报单仍需已确认的会话和合法字段，
但在风控规则中只会被 `HALT_ALL`（以及已配置的平仓限频器）拦截，因此缺少参考价或元数据时退出仍然可用。

`maxNotional` 只对 CTP 期货/TAS 产品类启用：期权权利金或组合价格不会冒充期货名义价值，产品类别
未知/不支持时，启用的名义价值闸会 fail-closed。名义价值按 `max(委托价, 新鲜参考价)` 估值，避免卖开
低限价低估。必须通过 [`trackMarketData()`](#tdtrackmarketdatamd--this) 接入行情；参考价缺失或超过
`maxReferenceAgeMs` 时，偏离与名义价值校验对开仓报单 fail-closed。

```ts
// 全套
td.setRiskLimits({
  maxOrderVolume: 10,          // 单笔开仓 ≤ 10 手
  maxPriceDeviation: 0.02,     // 距参考价 ≤ 2%
  maxNotional: 5_000_000,      // Futures/TAS 单笔开仓名义价值 ≤ 5M
  maxOrdersPerSec: 20,         // 令牌桶限频
  orderBurst: 40,              // 桶容量（默认 = maxOrdersPerSec）
  maxCloseOrdersPerSec: 10,    // 可选：独立平仓限频
  closeOrderBurst: 20,
});

// 只设几项（其余保持关闭）
td.setRiskLimits({ maxOrderVolume: 5, maxOrdersPerSec: 10 });

// 传 0 关闭某控制
td.setRiskLimits({ maxNotional: 0 });
```

#### `td.reduceOnly()` / `td.haltAll()` / `td.resume()` → `this`

原子模式为 `NORMAL`、`REDUCE_ONLY`、`HALT_ALL`。`reduceOnly()` 拦截普通开仓、全部高级变更请求和
银期转账，但保留撤单与单腿普通平仓；`haltAll()` 还会拦截平仓报单，直到 `resume()`。
模式切换与每笔报单共用最终 CTP API 发送屏障，因此调用返回后不会再发出被禁止的报单。
`resume()` 恢复 `NORMAL`。

复杂 insert、改单、对冲设置、套利/套保确认无法用逐笔规则判断，因此只在 `NORMAL`、会话已确认、且未配置
任何本地风控（限制或限频器）时转发；否则以 `-10006` 拒绝。生成器要求未来新增的每个非查询 SDK 请求先
完成分类；未审核方法会让代码生成失败，而不是默认无守卫放行。

这类请求逐个判断，不留下任何状态。高级请求——以及其他终端的报单——带来的敞口，原生闸门不跟踪；
核算这部分敞口是你交易层的事。

银期转账同样适用此规则。它会改变可用担保资金，因此只在 `NORMAL`、会话已确认、且所有本地风控关闭时
放行。调用方仍须自行承担鉴权与转账授权。

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

#### `td.trackMarketData(md)` → `this`

提供价格偏离与名义价值校验所使用的实时参考价。把这路 `MarketData` 的 C++ 快照缓存直接接入本
Trader 的风控引擎，使参考价**在 C++、于报单发送路径上**读取——无 JS 往返。未接入参考价或行情超过
`maxReferenceAgeMs`（默认 3000 ms）时，已启用的偏离或名义价值校验对开仓报单 fail-closed。

```ts
td.setRiskLimits({ maxPriceDeviation: 0.02 });

// 从行情实时取参考价（每笔报单都在 C++ 读取）
td.trackMarketData(md);
```

> 合约乘数与产品类别（用于 `maxNotional`）**不**从 JS 喂入：C++ 风控引擎直接从 CTP 的
> `OnRspQryInstrument` 回调里取，`reqQryInstrument` 的末包无错误到达时即刷新它们（见
> [`syncMultipliers`](#tdsyncmultipliersinstrumentids--promisenumber)）。原生闸门完全不使用保证金率；
> [`syncMarginRates()`](#tdsyncmarginratesinstrumentids-hedgeflag--promisenumber) 与 `readMarginRates()`
> 只为你自己的计算解析它们。缺失元数据不会被猜测。

#### 账户事实与账户级上限

原生闸门不维护持仓、挂单或资金账本，也没有账户级上限。CTP 的持仓快照与实时报单/成交流之间没有共同
序号，绑定内部的账本只能重复你自己的账本——还可能与之不一致。请把账户事实放在你的交易层，并在调用
`reqOrderInsert()` 之前于该层判断账户级限制（账户保证金、单合约保证金、持仓手数）：

- **快照**：每次 `session()` 之后（首次连接和每次重连）查询 `reqQryInvestorPosition`、`reqQryOrder`
  和 `reqQryTradingAccount`。
- **保持最新**：用 `rtn-order` / `rtn-trade`。CTP 也会投递该投资者其他会话的回报，所以同账户另一终端
  下的单也会出现在这里。快照与实时流之间的对账由你的账本负责。
- **保证金估算**：对即将发出的报单，乘数来自 `reqQryInstrument`，费率来自
  `syncMarginRates()` / `readMarginRates()`。

```ts
const acct = { brokerId: "9999", investorId: "id" };
let ready = false;
td.on("front-disconnected", () => { ready = false; }); // 断线即停止新增敞口
td.on("front-connected", async () => {
  await td.session({ brokerId: acct.brokerId, userId: acct.investorId, password: "pw",
    sync: { marginRates: { instrumentIds: ["rb2510"], hedgeFlag: "1" } } });
  // 本经纪商、本币种的资金行必须恰好一行。资金行不含投资者号，资金账号可与投资者号不同：按柜台返回的保留。
  const funds = (await td.reqQryTradingAccount({ ...acct, currencyId: "CNY" }))
    .filter((f) => f.brokerId === acct.brokerId && f.currencyId === "CNY");
  if (funds.length !== 1) throw new Error("expected exactly one CNY funds row");
  book.reset(await td.reqQryInvestorPosition(acct), await td.reqQryOrder(acct), funds[0]);
  ready = true;
});
td.on("rtn-order", (o) => book.onOrder(o));
td.on("rtn-trade", (t) => book.onTrade(t));

// 先做你自己的账户级检查，再交给原生逐笔闸门
const [rb] = td.readMarginRates(["rb2510"], "1");
const { volumeMultiple } = specs.get("rb2510"); // 取自你自己核对过的合约目录
const add = 3500 * 1 * volumeMultiple * rb.longMarginRatioByMoney + 1 * rb.longMarginRatioByVolume;
if (ready && book.marginInUse() + add <= MY_ACCOUNT_MARGIN_CAP && book.longLots("rb2510") + 1 <= MY_LOT_CAP) {
  await td.reqOrderInsert({ instrumentId: "rb2510", direction: "0", combOffsetFlag: "0", limitPrice: 3500, volumeTotalOriginal: 1 });
}
```

`book` 代表你自己的账本；本绑定不提供它。

### `td.apiVersion` / `td.tradingDay` → `string`（只读）

CTP 交易 API 版本与当前交易日。

```ts
console.log("CTP 交易 API", td.apiVersion, "交易日", td.tradingDay);
```

### `td.frontInfo` → `FrontInfo`（只读）

所连前置及其给本会话的每秒流控：`qryFreq` 为查询笔数，`ftdPkgFreq` 为 FTD 报文数（登录、查询、报单、撤单合计，
超出的报文由前置缓存到下一秒、不报错）。登录后有效；查询间隔自动据此计算，前置变化或登录、登出时恢复为 1100 ms。

```ts
const { frontAddr, qryFreq, ftdPkgFreq } = td.frontInfo;
console.log(frontAddr, "查询/秒", qryFreq, "报文/秒", ftdPkgFreq);
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

另外每个请求方法都有对应的 `rsp-*` 事件（如 `rsp-qry-investor-position`）。原始 `rsp-*` 事件按到达
原样转发每一个应答回调，包括已失败、已超时、因重连被撤销或被新请求取代的请求的迟到行。它们只适合记日志和
诊断；合约规格、持仓、资金等状态只能从请求的 Promise 结果或同步助手建立，这些只在本请求完整且无错误的应答到达
时才完成。

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
- **其余所有请求** → `Promise<...>`，以其自身 `OnRsp*` 响应的首行 resolve；`reqSettlementInfoConfirm`
  返回全部回执行（`Promise<unknown[]>`），便于调用方逐行核对账户。

请求只会被它自己的响应回调（`ReqXxx` → `OnRspXxx`；各种登录方式 → `OnRspUserLogin`）或通用
`rsp-error` 结束。带着相同请求号的其他类型回调仍照常作为事件发出，但不会 resolve、reject 该请求，
也不会计入其结果行；若之后再无应答，请求按空闲超时失败，错误信息会写明被忽略的回调。

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
关闭**。`maxReferenceAgeMs` 必须为正数，省略时为 3000 ms；`NaN`/`Infinity` 抛错。每条规则只判断一笔
报单，没有账户级字段。

| 字段 | 类型 | 含义 |
|---|---|---|
| `maxOrderVolume` | `number` | 单笔开仓最大手数。 |
| `maxPriceDeviation` | `number` | 开仓报单的最大 `\|价 − 参考价\| / 参考价` 比例；参考价缺失或过期时 fail-closed。 |
| `maxReferenceAgeMs` | `number` | 偏离与名义价值校验允许的最大行情年龄；默认 3000 ms。 |
| `maxNotional` | `number` | 期货/TAS 单笔开仓最大名义价值（`max(委托价, 新鲜参考价) × 量 × 乘数`）；参考价缺失/过期、乘数缺失或其他产品类别时 fail-closed。 |
| `maxOrdersPerSec` | `number` | 每秒最大开仓报单发送数（令牌桶）。 |
| `orderBurst` | `number` | 令牌桶容量。默认：`maxOrdersPerSec`。 |
| `maxCloseOrdersPerSec` | `number` | 独立平仓发送速率；默认关闭。 |
| `closeOrderBurst` | `number` | 平仓令牌桶容量；默认 `maxCloseOrdersPerSec`。 |

```ts
const conservative: RiskLimits = { maxOrderVolume: 5, maxOrdersPerSec: 10, maxNotional: 2_000_000 };
td.setRiskLimits(conservative);
```

### `SessionOptions`

[`session`](#tdsessionopts--promise-identity-multipliers-marginrates-) 的选项。字段见那里的表格。

```ts
const opts: SessionOptions = {
  brokerId: "9999", userId: "id", password: "pw",
  appId: "simnow_client_test", authCode: "0000000000000000",
  confirmSettlement: true,
  sync: { multipliers: ["rb2510"], marginRates: { instrumentIds: ["rb2510"], hedgeFlag: "1" } },
};
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
// 仅作诊断：原始事件也会带来已失败或被取代请求的迟到行。账本请用 `await td.reqQryInvestorPosition(...)` 建立。
td.on("rsp-qry-investor-position", (row, options) => {
  if (options.rspInfo) console.error("请求", options.requestId, options.rspInfo.errorMsg);
  else if (options.isLast) console.log("请求", options.requestId, "末包");
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
| 风控模式 | `reduceOnly()` / `haltAll()` / `resume()` | 开仓或全部报单（撤单始终放行） | `resume()` |
| 单笔最大手数 | `setRiskLimits({ maxOrderVolume })` | 每笔开仓 | `0` / 省略 |
| 最大价格偏离 | `setRiskLimits({ maxPriceDeviation })` + 参考价 | 每笔开仓 | `0` / 省略（无参考价会拦截） |
| 最大名义价值 | `setRiskLimits({ maxNotional })` + 参考价 + 乘数 | 每笔开仓 | `0` / 省略 |
| 开仓限频 | `setRiskLimits({ maxOrdersPerSec, orderBurst })` | 每秒 | `0` / 省略 |
| 平仓限频 | `setRiskLimits({ maxCloseOrdersPerSec, closeOrderBurst })` | 每秒 | `0` / 省略 |
| 账户保证金、单合约保证金、持仓手数 | 你自己的交易层 | 账户 / 合约 | — |

原生控制都是无状态的：每条只依据当前这笔报单本身、最新参考价（`trackMarketData`）和静态合约元数据
（`reqQryInstrument`）判断，因此都不需要持仓或挂单快照，重连后也不会改变。它们全部在 C++ 发送路径上
执行；突破会让 `reqOrderInsert()` reject，原因在消息里。依赖账户持仓或挂单的限制在你自己的交易层判断——
见[账户事实与账户级上限](#账户事实与账户级上限)。

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

// 1) 在任何报单能发出之前先配置风控（逐笔闸门，在 C++ 中执行）。
td.setRiskLimits({ maxOrderVolume: 5, maxNotional: 2_000_000, maxOrdersPerSec: 10, maxPriceDeviation: 0.02 });
td.trackMarketData(md);              // 偏离与名义价值闸共用的新鲜参考价
const MAX_NET_LOTS = 10;             // 账户级上限：在这里、在你自己的交易层判断

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

// 4) 持仓由「成交」推导，绝不凭发送臆测。（真实账本还应在每次 session() 之后
//    从 reqQryInvestorPosition 起步；见重连示例。）
let position = 0; // 净手数（+多 / -空）
td.on("rtn-trade", (t: Trade) => {
  position += t.direction === Direction.Buy ? t.volume : -t.volume;
  console.log("成交", t.price, "x", t.volume, "-> 净持仓", position);
});

// 5) 策略：把每个 tick 转成目标，并交易差额。
md.on("rtn-depth-market-data", async (tick: DepthMarketData) => {
  if (tick.instrumentId !== SYMBOL) return;
  // 你的逻辑：目标净手数，并夹在你自己的持仓上限内
  const want = Math.max(-MAX_NET_LOTS, Math.min(MAX_NET_LOTS, signal(tick)));
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
const acct = { brokerId: CREDS.brokerId, investorId: CREDS.userId };
td.on("front-connected", async () => {
  // session() 会重新认证、重新登录、重新确认结算单、重新同步乘数，并向原生报单闸门重新确认账户
  // （确认前报单会被拒）。闸门本身没有要重建的东西。
  await td.session({ ...CREDS });
  // 但断线期间你的账本可能漏掉了报单/成交回报：用新快照重建，此后继续应用 rtn-order / rtn-trade。
  book.reset(await td.reqQryInvestorPosition(acct), await td.reqQryOrder(acct));
});

td.on("front-disconnected", (reason) => {
  console.warn("交易链路断开（码", reason, "）—— CTP 会自动重连");
  // 在途请求 Promise 已经 reject；若你的策略需要已确认的会话，
  // 就暂停发新信号，直到下一次 front-connected。
});
```

CTP 的持仓/委托快照与实时回报流之间没有共同序号，二者的对账属于你的账本（本绑定有意不保留自己的
副本）。这里不要用裸 `reqUserLogin()` 代替 `session()`：只有 `session()` 会为报单确认账户。

### 组合与多腿下单

**平今 vs 平昨。** SHFE/INE 区分两者，所以光用 `"平仓"` 不够——要选 `CloseToday`（`"3"`）或
`CloseYesterday`（`"4"`）。要平掉 5 手多头（其中 2 手是今仓、3 手是历史仓），发两笔：

```ts
import { Direction, OffsetFlag } from "@hitrading/ctp-node";

await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseToday,     limitPrice: px, volumeTotalOriginal: 2 });
await td.reqOrderInsert({ instrumentId: "rb2510", direction: Direction.Sell, combOffsetFlag: OffsetFlag.CloseYesterday, limitPrice: px, volumeTotalOriginal: 3 });
```

**多腿开平标志明确不支持。** 原生闸门按“一个合约、一个限价”判断，无法判断 `"00"`、`"30"`、`"03"`
等值的逐腿价格与数量。因此 `reqOrderInsert()` 要求恰好一个开平字符，多腿模板会在到达 CTP 前被拒绝。
需要组合合约时，应在具备逐腿核算的更高层网关中实现。

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
