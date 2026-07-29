<!-- LANG-SWITCH -->
<p align="right"><a href="README.md">English</a> · <b>简体中文</b></p>

<h1 align="center">ctp-node</h1>

<p align="center">
  <b>面向 Node.js 的高性能、类型安全的 <a href="http://www.sfit.com.cn/">CTP</a>（上期技术）绑定</b><br/>
  用于程序化 / 量化期货交易。
</p>

<p align="center">
  <a href="https://github.com/hitrading/ctp-node/actions/workflows/build.yml"><img alt="CI" src="https://github.com/hitrading/ctp-node/actions/workflows/build.yml/badge.svg"></a>
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/github/license/hitrading/ctp-node?color=blue"></a>
  <img alt="Node" src="https://img.shields.io/badge/node-22.22.2%2B-brightgreen">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey">
  <img alt="TypeScript" src="https://img.shields.io/badge/TypeScript-strict-3178c6">
  <img alt="decode" src="https://img.shields.io/badge/decode-~8M%20ticks%2Fs-ff7f0e">
</p>

<p align="center">
  <img src="docs/assets/validation.svg" alt="ctp-node 验证 —— 离线套件 + 明确标注的历史实盘样本" width="780">
</p>

- **纯对象、地道的 TypeScript。** 每个 CTP 结构体都是生成的 `interface`，字段为 camelCase（`tick.lastPrice`、`tick.instrumentId`）；每个 CTP 枚举都是真正的 TS `enum`。无手写编解码。
- **快。** 独立的 46 字段生成解码器约为 **800 万 tick/秒**。更重要的是，可复现的原生 SPI→JS 全路径探针在 18,111 个真实缓存 key 下以 50 万 tick/秒持续运行且 0 丢弃，并完整接住一次 18,111 条全合约回调洪峰；硬件、CPU/RSS 与边界见[性能与内存](docs/troubleshooting.zh-CN.md#性能与内存)。
- **混合 API。** 流式推送用 `EventEmitter`（`rtn-depth-market-data`、`rtn-order` 等）+ 请求/响应用 `Promise`（登录、查询、报单），按 request id 关联并支持多行累积。
- **盘前风控在 C++ 中。** 一键熔断、单笔最大手数、最大名义价值、价格偏离（防胖手指）、账户级与按合约的开仓持仓保证金上限、按合约的最大持仓（手数、分边）、以及令牌桶限频器，都在原生代码里、报单路径上执行——JS 的 GC 停顿打不垮它们。
- **构造即稳定。** 无手写编解码、threadsafe-function 正确释放、`Init()` 延后以保证 `front-connected` 不丢失、编译器真值（`offsetof`）的二进制布局、GB18030 在 JS 解码（无 Windows 代码页 bug）。

> 面向 CTP 实际支持的范畴（快照驱动、容忍毫秒级的策略：CTA、套利、做市）。真正的微秒级 HFT 在任何语言下都无法通过 CTP 实现。

> **边界：** 本包是进程内协议 binding 与第二道本地风控，不负责 caller 鉴权、签名 mandate 或外部
> 心跳看门狗。生产使用仍需有鉴权的网关/监督进程，并对确切 commit 与打包二进制重新浸泡验证。
> 协议接口范围大于本地账本：定量准入只对普通单腿报单形成闭环；quote/exec/comb/parked 等高级变更
> 仅属于 binding 能力，启用本地风控时会拒绝。在确切的 `3.0.0-rc` 制品完成浸泡且外部
> mandate/watchdog 落地之前，生产实盘判定仍为 **No-Go**。

## 环境要求

受支持的偶数版本线要求 Node 22.22.2+（`^22.22.2 || ^24.15.0 || >=26.0.0`）。该范围与内置 `node-gyp` 的 Node.js 要求一致；不支持奇数/EOL 版本及 Node 24.0–24.14。**Windows 与 Linux（x64）提供预编译二进制**，这两个平台多数用户无需编译器。**macOS（及其他平台）在安装时从源码构建**，需要 C++ 工具链（macOS：Xcode Command Line Tools；Windows：VS Build Tools；Linux：clang/gcc）。CTP 共享库随插件一起打包在旁并自动解析。

## 安装

```sh
npm install @hitrading/ctp-node
```

支持 **TypeScript 或 JavaScript**、**ESM（`import`）或 CommonJS（`require`）**——包内同时发布两种构建产物与 `.d.ts` 类型声明，按你项目的模块设置自动加载对应格式：

```ts
import { Trader, MarketData } from "@hitrading/ctp-node";       // ESM / TypeScript
```
```js
const { Trader, MarketData } = require("@hitrading/ctp-node");  // CommonJS
```

## 文档

- **完整 API 参考** —— 每一个 TypeScript/JavaScript 接口和方法，含每个参数的类型与含义，以及用法示例：[English](docs/API.md) · [简体中文](docs/API.zh-CN.md)
- 架构与原生内部 —— 无锁数据平面、C++ 风控引擎、背压，以及一次创建的进程生命周期说明：[English](docs/native-hooks.md) · [简体中文](docs/native-hooks.zh-CN.md)
- **故障排查与 FAQ** —— 连接/前置轮换、登录与认证、查询限频、报单被拒、跌停、生命周期、构建问题、CTP 错误码：[English](docs/troubleshooting.md) · [简体中文](docs/troubleshooting.zh-CN.md)
- **测试与覆盖率** —— 完整离线回归 + 需要凭据、必须另行执行的 SimNow 集成套件；历史样本不等于当前候选版浸泡结论：[集成与测试](docs/integration.md)
- **发布** —— 全自动 `semantic-release`（Conventional Commits → 版本号 + CHANGELOG + GitHub Release + npm 发布）：[RELEASING.md](RELEASING.md) · [CHANGELOG.md](CHANGELOG.md)

## 快速开始 —— 行情

```ts
import { MarketData } from "@hitrading/ctp-node";

const md = new MarketData("./flow/md/", "tcp://182.254.243.31:30012");

md.on("front-connected", async () => {
  await md.login({ brokerId: "9999", userId: "xxxx", password: "xxxx" });
  md.subscribe(["rb2510", "ag2512"]);
});

md.on("rtn-depth-market-data", (tick) => {
  // tick 是带 camelCase 字段的纯对象
  console.log(tick.instrumentId, tick.lastPrice, tick.bidPrice1, tick.askPrice1);
});

// MD 成功登录后，最新行情缓存在 C++ 中（一个 last-value 缓存，当前认证
// 会话内的 tick 到达 JS 前更新），因此无需在 JS 里记账即可同步取到快照：
//   md.snapshot("rb2510");  // 最新完整深度行情（DepthMarketData）或 null
//   md.last("rb2510");      // 最新价，尚无则为 0
```

## 快速开始 —— 交易

```ts
import { Trader } from "@hitrading/ctp-node";

const td = new Trader("./flow/td/", "tcp://182.254.243.31:30002");

// 盘前风控，在 C++ 中对每笔报单执行：
td.setRiskLimits({ maxOrderVolume: 10, maxOrdersPerSec: 20, maxPriceDeviation: 0.02, maxAccountMargin: 2_000_000 });
td.trackMarketData(md); // 为偏离、名义价值与保证金闸提供 C++ 新鲜参考价
td.setInstrumentPositionLimits({ rb2610: 100, au2610: 10, ru2610: { long: 100, short: 20 } });
td.setInstrumentMarginLimits({ ag2608: 2_000_000, au2608: 5_000_000 });              // 按合约的计划保证金上限

td.on("front-connected", async () => {
  // 一站式握手。保证金表需显式给出有限交易合约集和套保标志（CTP 每合约一次查询）。
  await td.session({
    brokerId, userId, password, appId, authCode,
    sync: {
      marginRates: {
        instrumentIds: ["rb2510", "ag2608", "au2608"],
        hedgeFlag: "1",
      },
    },
  });

  // ……或在需要更细控制时手写：
  //   await td.reqAuthenticate({ brokerId, userId, appId, authCode });
  //   await td.reqUserLogin({ brokerId, userId, password });
  //   await td.reqSettlementInfoConfirm({ brokerId, investorId }); // 实盘账户
  //   await td.syncMultipliers(); await td.syncMarginRates(["rb2510"]);
  //   await td.syncOrders(); await td.syncPositions(); // 最后提交持仓快照

  // 基于 Promise 的查询返回所有行：
  const positions = await td.reqQryInvestorPosition({ brokerId, investorId });

  // 报单走 C++ 风控网关。reqOrderInsert 提交成功即 resolve（CTP 对已接受的报单
  // 不返回 ack）；通过 rtn-order / rtn-trade 跟踪结果。orderRef 留空会自动分配唯一值。
  await td.reqOrderInsert({
    brokerId, investorId, instrumentId: "rb2510",
    direction: "0", combOffsetFlag: "0", // 买开；见枚举
    limitPrice: 3500, volumeTotalOriginal: 1,
  });
});

td.on("rtn-order", (order) => console.log("报单更新", order.orderStatus));
td.on("rtn-trade", (trade) => console.log("已成交", trade.price, trade.volume));

// 应急一键熔断（拦截增险入口，保留减仓/撤单）：
// 原子模式：reduceOnly() 保留平仓与撤单；haltAll() 也拦平仓。
// 多腿 combOffsetFlag 在具备逐腿账本前会直接拒绝。
// td.reduceOnly();  /  td.haltAll();  /  td.resume();
```

### 预埋单（延迟攸关）

在市场触及你的触发条件的瞬间从 C++ 发出报单——在行情回调线程上判定，穿过风控网关，**JS 不在回路中**（无事件循环跳转、无 GC 暴露）：

```ts
const armed = trader.arm(md, {
  instrumentId: "rb2510",
  side: "buy",            // buy 在 ask ≤ trigger 时触发；sell 在 bid ≥ trigger 时触发
  triggerPrice: 3500,
  order: {
    brokerId, investorId, instrumentId: "rb2510", direction: "0", combOffsetFlag: "0",
    limitPrice: 3500, volumeTotalOriginal: 1, orderRef: "900001",
  },
});
// 成功发送后删除；拒绝会保留并按有上限的指数退避重试。
// armed.disarm();
```

枚举已导出且带类型：

```ts
import { Direction, OffsetFlag } from "@hitrading/ctp-node";
Direction.Buy;   // "0"
Direction.Sell;  // "1"
```

## API 形态

- `new MarketData(flowPath, fronts)` / `new Trader(flowPath, fronts)` —— `fronts` 是一个 `tcp://` 地址或数组。
- 请求是 camelCase 方法，接受 CTP 字段对象的 `Partial<...>` 并返回 `Promise`。请求必须是严格普通对象；未知、继承或已删除字段以及 `null` 字段值会直接报错。输入数组必须稠密且每项都是显式自有值；稀疏或由原型提供的项会被拒绝。`reqQry*` 以行数组 resolve；多数请求以单条响应行 resolve。
- 同一客户端内全部 `reqQry*` / `reqQuery*` 都会在调用时快照、内部串行并限频（最多一个在途，两次尝试发送至少间隔 1100 ms），调用方不要再写 `setTimeout(1000)`。有界队列按客户端隔离；原始查询只限频，只有 `sync*` 会重试瞬时流控错误。
- 发往交易所的 insert/action（`reqOrderInsert`、`reqOrderAction` 等）**提交成功即** resolve —— CTP 对已接受的报单不返回成功响应，只有 `rtn-order` / `rtn-trade`（用 `orderRef` 关联）。只有发送被拒（风控网关、限频、或 CTP API 错误码）才 reject。
- 流式事件用 kebab-case 名称（`rtn-depth-market-data`、`rtn-order` 等）；处理函数收到 `(data, options)`，`options` 带 `{ requestId?, isLast?, rspInfo? }`。
- 每个合约的最新 tick 保存在 C++ 的 last-value 缓存里。成功 MD 登录会开启一个全新的空 epoch；此前，以及连接/断线、新登录或登出尝试、成功登出、关闭之后，缓存都会立即失效并清空。只有当前已认证 epoch 后续收到的 tick 才能更新缓存或驱动预埋单。`md.snapshot(instrumentId)` 同步返回最新完整 `DepthMarketData`（或 `null`），`md.last(instrumentId)` 返回最新价（无则为 0）。价格偏离校验使用同一份带 epoch 约束的缓存。
- 行情背压使用有界 keep-newest 暂存并保留最新行情，不覆盖 JS 正在解码的槽位；交易端 4096 槽满后进入有序无损队列，不主动丢回报，但持续消费停顿会增长内存。
- `client.close()` 释放底层 CTP API。在关闭时调用**一次**——CTP 客户端是一次创建/复用的；靠重建来重连会在厂商 DLL 里死锁（见[生命周期](#生命周期-一次创建并复用)）。

## 生命周期 一次创建并复用

`MarketData` / `Trader` 只创建**一次**并持有到进程结束。CTP 会自行重连；通过在 `front-connected` 再次触发（首次连接 *以及* 每次自动重连都会触发）时原地重跑握手来**原地**处理断线——绝不要销毁再重建客户端。

```ts
const md = new MarketData("./flow/md/", front);
md.on("front-connected", async () => {     // 首次连接和每次重连
  await md.login({ brokerId, userId, password });
  md.subscribe(["rb2510"]);
});
// Trader 同样：原地重跑一站式握手
td.on("front-connected", () => td.session({ brokerId, userId, password, appId, authCode }));
```

> **为什么。** Windows 下反复 `Init()`/`Release()` 已观察到会卡在厂商 DLL 的 `Release()`；进程无法安全恢复。每个客户端只构造一次，且只在关闭时 `close()`。若一个进程内创建/关闭了异常多的客户端，会打印一次性的 `console.warn`（用 `CTP_RECREATE_WARN` 调节/禁用）。详见 [docs/native-hooks.zh-CN.md](docs/native-hooks.zh-CN.md#进程生命周期-一次创建并复用)。

## 架构

```
CTP 回调线程 (C++)                          Node 事件循环 (JS)
  OnRtn.../OnRsp... → memcpy 字节   ──►    合并的门铃 → 整批排空
  进无锁 SPSC 环、                           → 从环里逐条解码
  自增原子索引、敲门铃                          （单态生成的解码器）
  (溢出时：行情暂存并淘汰最旧、                 → 纯对象 → emit / resolve
   交易端有序无损溢出)
```

公开 API 之下的一切都由 CTP 头文件（`ctpsdk/ThostFtdc*.h`）经 `scripts/codegen/` 生成—— 519 个结构体接口、326 个枚举、字段布局表（经 `offsetof`），以及完整的 trader SPI + 请求分发。更新头文件后运行 `npm run gen`。

## 从源码构建

```sh
npm run gen      # 从 CTP 头文件重新生成
npm run build    # gen + 原生 (node-gyp) + tsc
npm test         # 编解码往返 + md + trader（无需网络）
npm run bench    # 解码吞吐
```

## 许可证

Apache-2.0
