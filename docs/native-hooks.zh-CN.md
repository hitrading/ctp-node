<!-- LANG-SWITCH -->
[English](native-hooks.md) · **简体中文**

# 原生可下沉钩子（风控 / 限频 / 预埋触发）

目标：把约 95% 的策略工作留在 JS/TS（迭代快、生态好），同时给**延迟与安全攸关**的那几片
C++ 级确定性——任何 GC 停顿或事件循环卡顿都打不垮它们。JS 定规则；C++ 执行并落地。

## 两个世界

| | C++ 世界 | JS 世界 |
|---|---|---|
| 运行于 | CTP 回调线程 | Node 主线程（事件循环） |
| GC 停顿 | 无 | 有（偶发，毫秒到几十毫秒） |
| 确定性 | 微秒级、可预测 | 抖动（GC / JIT / 调度） |

一个 tick 首先到达 **C++ 回调线程**。纯 JS 策略必须跨到 JS 并等待事件循环——吃下那份抖动。
“下沉”把关键决策留在 C++ 线程；JS 只做配置，并在事后被通知。

## 下沉了什么

1. **风控（盘前）** — `src/native/risk.h` `RiskEngine`
   每次发送前都跑硬性检查：一键熔断、单笔最大手数、价格偏离（防胖手指）、最大名义价值、开仓
   持仓成本上限（账户级与按合约）、以及按合约的最大持仓（手数，按边封顶）。在 C++ 里执行，
   因此即使 JS 进程正在 GC、被阻塞或有 bug，它们依然成立。*安全收益——即便慢策略也值得。*
   - 状态：**全部为实**。价格偏离用一个按合约的参考价，由行情喂入（`setRefPrice` /
     `trackMarketData`）。名义价值与持仓成本都套用合约乘数（价 × 量 × 乘数）。持仓成本按合约
     跟踪 Σ(开仓成本)（多/空，平仓时按比例释放），在交易回调线程上由 `OnRtnTrade` 更新。
   - 乘数与已有持仓自动从 CTP 拉取：`syncMultipliers()`（经 `reqQryInstrument`）与
     `syncPositions()`（经 `reqQryInvestorPosition`）——无需手动 `setMultiplier` / `seedPosition`
     （它们保留供测试/离线使用）。乘数与持仓状态分开存储，因此 `syncPositions()`（会重置并重新
     播种持仓）绝不会清掉乘数。
   - 在途预约：持仓上限按 `held + working`（已发但未成交的报单）计算，使一连串开仓不会在成交
     回报到达前溜过。预约按 `(FrontID, SessionID, OrderRef)` 跟踪，并由 `OnRtnOrder` 对账（自我
     纠正；在成交/撤单/拒单及发送失败时释放）。由于 CTP 会把一个投资者的报单/成交回报投递到其
     *所有*会话，这也把**同账户另一终端**下的报单算进去（它们的成交按合约更新持仓；它们的挂单
     占用上限）。`syncOrders()`（经 `reqQryOrder`）从经纪商的挂单重建预约——登录后和任何重连后
     都要调用。

2. **限频** — `src/native/risk.h` `RateLimiter`
   令牌桶，保证席位永不超过交易所/CTP 的报单速率，对停顿后的 JS 突发免疫。
   - 状态：**实**（令牌桶）。

3. **预埋（延迟攸关触发器）** — `src/native/arm.h` `ArmRegistry`
   一条窄规则（“ask ≤ X 的瞬间就发”），在 MD 回调线程上判定，由 JS 参数化。仅用于真正延迟
   敏感的策略。
   - 状态：**已接通**。`trader.arm(md, spec)` 与喂 tick 的 MarketData 共享同一注册表（shared_ptr）；
     命中时报单由 JS 编码的模板构建，穿过该 Trader 的 `RiskEngine`，在 MD 回调线程上经
     `ReqOrderInsert` 发出（无 JS）。一次性；回执经普通交易事件到达。拆除安全：在释放 API 之前，
     会在注册表锁内清空 sink。

## 线程 / 安全模型

- 配置写在**慢路径**（JS 线程），读在**热路径**（JS 或回调线程）。风控配置用**原子量**（无锁
  读取）；限频器用一个极小的互斥锁（有界、无争用）。
- `ArmRegistry` 目前用互斥锁守护其列表；TODO 是改成读多型无锁快照，使 `onTick` 永不因罕见的
  arm/disarm 写入而阻塞。

## 日志 / 可观测性

刻意地，C++ 端**不做同步日志**——在 CTP 回调线程上做阻塞 I/O 或字符串格式化，正好会引入本设计
要避免的延迟/抖动。热路径只保留 memcpy + 原子量。可观测性按低延迟系统的惯常做法分层：

- **C++ 里用计数器/事件，而非日志。** `droppedRecords`（背压——行情丢最旧的记录以保留最新报价，
  交易端丢最新的，使排队中的报单/成交回报不丢失）、预埋触发计数，以及流式事件（`front-connected`、
  带原因的 `front-disconnected`、带 `errorId`/`errorMsg` 的 `rsp-*`）都暴露给 JS。
- **原因被上抛而非吞掉。** 盘前风控拒单会把具体原因带回 JS（如 `blocked by pre-trade risk:
  order volume exceeds maxOrderVolume`；持仓上限和限频有各自的消息），使日志能说清*为什么*。
  在热路径上零成本——它只在（罕见的）拒单时设置。
- **真正的日志在 JS 里做**，离开回调线程，从那些事件出发——任何异步日志库（pino/winston）都行。
  若将来确需 C++ 端审计日志，加一个可选的异步/延迟日志器（无锁队列 + 后台线程，类似
  NanoLog/Quill）——绝不要同步的。

## 公开的 TS 接口（外壳）

这些全部暴露在 `Trader` 上（`src/trader.ts`）：

```ts
// 盘前风控（发布给 C++ 执行器；立即生效）：
td.riskSet({ maxOrderVolume: 5, maxPriceDeviation: 0.02, maxOrdersPerSec: 20, maxPositionCost: 5_000_000 });
td.trackMarketData(md);                   // 喂入偏离/名义价值的参考价
td.setMaxPositions({ rb2610: 100, ru2610: { long: 100, short: 20 } }); // 按合约手数上限
td.setMaxPositionCosts({ ag2608: 2_000_000, au2608: 5_000_000 });      // 按合约成本上限
td.halt(); td.resume();                   // 一键熔断（C++ 立即拦截所有发送）

// 登录后自动从 CTP 拉取的风控输入：
await td.syncMultipliers();               // 合约乘数（reqQryInstrument）
await td.syncPositions();                 // 已有开仓成本（reqQryInvestorPosition）

// 延迟攸关的预埋触发器（从 C++ 在 MD 线程上触发）：
const armed = td.arm(md, {
  instrumentId: "rb2510", side: "buy", triggerPrice: 3500,
  order: { /* InputOrder；orderRef 留空会自动分配 */ },
});
armed.disarm();                           // 移除触发器
// 一次性；成交/回执经普通的 rtn-order / rtn-trade 事件到达。
```

## 状态

已完全接通并在 SimNow 上实测验证：报单发送路径上的风控网关（手数 / 偏离 / 名义价值 / 持仓成本 /
熔断）与限频器，由 MD 回调喂入、无 JS 跳转地经风控网关触发 `ReqOrderInsert` 的
`ArmRegistry::onTick`，按合约的持仓状态，以及按乘数精确的名义价值 + 持仓成本。

## 进程生命周期 一次创建并复用

`MarketData` 和 `Trader` 是**一次创建、进程内一直复用**的对象。包装构造函数调用原生 `_start()`，
后者调用厂商 `Init()`；`close()` 调用厂商 `Release()`（`CThostFtdcMdApi::Release()` /
`CThostFtdcTraderApi::Release()`——见 `src/native/mdapi.cc` 和 `src/native/traderapi.cc` 里的
`doClose()`）。

**隐患。** CTP 厂商 DLL（`thostmduserapi_se.dll` / `thosttraderapi_se.dll`）在一个进程内做几次
`Init()`→`Release()` 循环后会**在 `Release()` 内部死锁**——经验上 4–11 次，随时机和 CTP 内部
重连线程状态而变。这是 CTP SDK 本身的已知限制：其 API 对象*不能*在一个进程内反复
create / `Init` / `Release`。这**不是**本绑定 C++/TS 的 bug，也**无法在此修复**——卡死在闭源
厂商 DLL 内部。（佐证：*不*调用 `_start()` 的裸原生对象能扛过 80+ 次 create/close 循环；触发点
正是 `Init()`+`Release()` 这一对。一个永不连上的前置（如 `tcp://127.0.0.1:1`）会让它更糟，因为
卡住的正是 CTP 的重连机制。）

**规则：绝不要靠销毁再重建客户端来重连。** CTP 会自行重连。**原地**处理断线：当 `front-connected`
再次触发时（首次连接 *以及* 每次自动重连都会触发），在同一实例上重新跑握手。

```ts
const md = new MarketData("./flow/md/", front);   // 只构造一次
md.on("front-connected", async () => {            // 首次连接和每次重连
  await md.login({ brokerId, userId, password }); // 原地重新登录
  md.subscribe(["rb2510"]);                        // 原地重新订阅
});

// Trader 同理——重连时重新跑一站式握手：
td.on("front-connected", () => td.session({ brokerId, userId, password, appId, authCode }));
```

`close()` 只在进程关闭时调用**一次**（或干脆不调用——进程退出会干净地拆除一切）。
reconnect-by-recreate 循环则会把整个进程卡死在厂商 DLL 内。

**Windows 上卡死时的连带后果。** 卡死的进程会忽略 `SIGTERM`（卡住的 CTP 线程从不观测到信号），
所以超时/被杀的 Node 进程会**残留**，必须强杀：`taskkill /F /PID <pid>`。在它死掉之前会一直占着
`build\Release\ctp.node`，于是下次原生构建无法覆盖该插件，报 `LNK1104: cannot open file ...
ctp.node`。如果构建忽然写不了插件，先找一个游离的 `node` 僵尸进程并 `taskkill /F` 掉它。

**开发期绊线。** 作为防止 reconnect-by-recreate bug 溜进生产的兜底，`CtpClient` 维护一个进程级
计数：被启动（`Init`）后又关闭（`Release`）的客户端数，越过阈值（默认 3——刻意低于约 4 次的死锁
点，使警告在典型卡死前的那次 `close()` 上打印；更高的值就来不及触发了）时打印一次性的
`console.warn`。它只在 `close()` 上运行——绝不在热数据路径上——故在延迟攸关处零成本，而一个
create-once 的应用（一两个长期存活、关闭时才 close 的客户端）永不触发它。用 `CTP_RECREATE_WARN`
环境变量调节或禁用：`CTP_RECREATE_WARN=0` 禁用；任意正整数设置阈值。

## 背压与行情响应

行情环是 `DropPolicy::Oldest`：当 JS 消费者落后一整环（8192 条记录）于 tick 洪流时，生产者会
覆盖最旧的未读槽。请求**响应**（`OnRspUserLogin` / `OnRspUserLogout` /
`OnRspQryMulticastInstrument`）与流式 tick 共用这个环，因此若在事件循环排空前一条响应后面堆积了
≥ 8192 条 tick，该响应理论上可能被丢弃——等待中的 Promise 届时会在空闲超时（`requestTimeoutMs`，
默认 30 秒）上 reject 而非 resolve。

实践中这无害：`login()` 在启动时、任何订阅之前运行，此时没有 tick 在流动；唯一*可能*被卡在实时
满市场洪流之后的响应是 `logout()`（关闭时发出）和很少用的 `reqQryMulticastInstrument()`。该失败
是有界的（超时 reject，绝非静默卡死或损坏）且可恢复（重试）。如果你必须在重负载下盘中查询行情，
请预期可能超时并重试。**交易端**的环是 `DropPolicy::Newest`（可靠）：报单/成交回报和所有交易端
响应在背压下永不丢弃，故无论行情负载如何，订单流的正确性都不受影响。
