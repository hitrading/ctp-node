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
   每次发送前都跑硬性检查：一键熔断、单笔最大手数、单笔最大名义价值（防胖手指）、价格偏离防护、
   开仓**保证金**上限（账户级与按合约）、以及按合约的最大持仓（手数，按边封顶）。在 C++ 里执行，
   因此即使 JS 进程正在 GC、被阻塞或有 bug，它们依然成立。*安全收益——即便慢策略也值得。*
   - 状态：**已实现，但产品模型有意收窄**。价格偏离防护的参考价来自 C++ 行情快照缓存（LVC；见下文 §4），经
     `trackMarketData(md)` 读取——无需 `setRefPrice`、无 JS 往返，且也覆盖预埋单。单笔名义价值
     检查仍是名义价值（价 × 量 × 乘数——防胖手指）。Futures/TAS 工作单保证金按 CTP 公式
     `价 × 量 × 乘数 × 按金额 + 量 × 按手`，并区分多空/套保标志；持仓快照使用 `UseMargin`，且按
     合约/多空/套保标志保留独立成本桶，使平仓只释放匹配桶。
     相对投资者表与匹配的交易所基础表逐字段相加。这是保守的本地闸门，不是所有券商保证金算法的
     通用复刻；金额闸启用时，不支持的产品类别会 fail-closed。
   - **架构原则：延迟敏感的元数据和实时账本事件直接从 SPI 回调进入 C++。** 权威持仓与工作单查询
     快照由 TypeScript 聚合，再通过带版本校验的 native 批操作提交。在
     `scripts/codegen/generate-trader.mjs` 中特判（生成进 `traderspi.gen.cc`）：
     - 合约乘数 ← `OnRspQryInstrument`（内部 `setInstrument`）
     - 按合约/套保标志的保证金表 ← `OnRspQryInstrumentMarginRate`。多空的按金额、按手字段分别保留。
       `OnRspQryExchangeMarginRate` 提供 `IsRelative` 所需基础表；乘数或任一所需表缺失时 fail-closed。
     - 登录会话（FrontID/SessionID，用于预约键）← `OnRspUserLogin`（`setSession`），登录时同步发布，
       使预约键在任何报单可发出之前就已正确。
     - 成交 ← `OnRtnTrade`（`onTrade`）
     - 报单更新 ← `OnRtnOrder`（`onOrderUpdate`）

     因此任何 `reqQryInstrument` 都会保持乘数最新，对你所交易的合约查询
     `reqQryInstrumentMarginRate` 可填充绝对保证金表——无需 JS 转交元数据。乘数与保证金表是静态元数据，与持仓状态
     分开存储，因此 `resetPositions()` / 一次 `syncPositions()` 重同步绝不会清掉它们。
   - 在途预约：持仓上限按 `held + working`（已发但未成交的报单）计算，使一连串开仓不会在成交
     回报到达前溜过。预约按 `(FrontID, SessionID, OrderRef)` 跟踪，并由 `OnRtnOrder` 对账（自我
     纠正；在成交/撤单/拒单及发送失败时释放）。终态墓碑还保存具体的交易日/交易所/合约/
     `OrderSysID` 指纹：同一交易所订单的迟到回放会被忽略；复用或无法判定的键会 fail-closed，直到
     权威 `syncOrders()` 快照取代旧墓碑。由于 CTP 会把一个投资者的报单/成交回报投递到其
     *所有*会话，这也把**同账户另一终端**下的报单算进去（它们的成交按合约更新持仓；它们的挂单
     占用上限）。
     Trade 幂等账本最多保留 65,536 个精确指纹，并优先退休完整的较旧交易日。如果单个活跃交易日
     自身耗尽该预算，实现不会淘汰含糊的键；开仓保持 fail-closed，直至观察到下一交易日并由
     `syncPositions()` 对账遗漏时段。
   - 仍由 JS 驱动（**非**自动获取）的部分：`seedPositions` / `syncPositions`（播种已有开仓）与
     `rebuildReservations` / `syncOrders`（经 `reqQryOrder` 从经纪商的挂单重建预约）。它们使用版本号
     校验的原子替换，保留工作单预约；若查询期间有实时回报就重试。故保持为显式、由 JS 编排的调用。
     登录后和任何重连后都要调用
     `syncOrders()`。

2. **限频** — `src/native/risk.h` `RateLimiter`
   令牌桶，保证席位永不超过交易所/CTP 的报单速率，对停顿后的 JS 突发免疫。
   - 状态：**实**（令牌桶）。

3. **预埋（延迟攸关触发器）** — `src/native/arm.h` `ArmRegistry`
   一条窄规则（“ask ≤ X 的瞬间就发”），在 MD 回调线程上判定，由 JS 参数化。仅用于真正延迟
   敏感的策略。
   - 状态：**已接通**。`trader.arm(md, spec)` 与喂 tick 的 MarketData 共享同一注册表（shared_ptr）；
     命中时报单由 JS 编码的模板构建，穿过该 Trader 的 `RiskEngine`，在 MD 回调线程上经
     `ReqOrderInsert` 发出（无 JS）。成功提交后立即删除；熔断/限频/元数据/发送拒绝时保留并退避重试；
     回执经普通交易事件到达。拆除安全：在释放 API 之前，
     会在注册表锁内清空 sink。

4. **行情快照（最新值缓存）** — `src/native/snapshot.h` `SnapshotCache`
   一个 C++ 最新值缓存（LVC），按合约保存最新的完整 `DepthMarketDataField`。成功 MD 登录会激活一个
   全新的空 epoch；只有当前认证 epoch 后续的 tick 才会在发布前更新它。连接/断线、登录/登出转换和
   关闭都会使缓存立即失效，因此重连后旧报价不可能伪装成新鲜风控参考价。退订按合约清除（`erase`）。
   - **JS 同步读取**：`md.snapshot(id)` 返回最新的完整 tick（非活动 epoch、从未见过或已退订则为 `null`），
     `md.last(id)` 只返回最新价——无需等待 tick，策略里也无需逐 tick 记账。
   - **偏离参考价完全在 C++ 内由 MD→风控流转。** Trader 的价格偏离检查经 `trackMarketData(md)`
     在 C++ 里读取该缓存：MarketData 以 `shared_ptr` 共享该缓存（与预埋单注册表的共享方式如出一辙），
     故风控引擎直接从 MD 馈源读取实时价，无 JS 往返——因此该检查也覆盖那些从 C++ 触发、回路中无 JS
     的预埋单。未认证或断线后的 tick 仍作为协议事件可见，但不能更新缓存或驱动预埋 mandate。
     （`SnapshotCache` 实现了 `RefPriceSource`；风控引擎经 `setMdSnapshot` 持有它。）

## 线程 / 安全模型

- 配置写在**慢路径**（JS 线程），读在**热路径**（JS 或回调线程）。风控配置用**原子量**（无锁
  读取）；限频器用一个极小的互斥锁（有界、无争用）。
- `ArmRegistry` 按合约建立索引，因此 `onTick` 只扫描该合约的活跃触发器（成功触发后立即删除）。
  短互斥区保证 arm/disarm、在途发送与 Trader teardown 线性一致。
- 会写队列或改变测试状态的钩子（`_injectTestTick`、`_injectTestForQuote`、
  `_injectTestLifecycle`、`_burstTicks`、`_produceTestTicks`、`_burstEvents`）默认禁用，只有显式设置
  `CTP_NODE_TEST_HOOKS=1` 才可调用。实盘进程绝不能开启：EventChannel 依赖厂商回调流是唯一生产者，
  而生命周期注入还会刻意绕过真实前置。

## 日志 / 可观测性

刻意地，C++ 端**不做同步日志**——在 CTP 回调线程上做阻塞 I/O 或字符串格式化，正好会引入本设计
要避免的延迟/抖动。热路径只保留 memcpy + 原子量。可观测性按低延迟系统的惯常做法分层：

- **C++ 里用计数器/事件，而非日志。** `droppedRecords`（行情安全淘汰最旧逻辑记录；交易端有序无损
  溢出）、预埋触发计数，以及流式事件（`front-connected`、
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
td.setRiskLimits({ maxOrderVolume: 5, maxPriceDeviation: 0.02, maxOrdersPerSec: 20, maxAccountMargin: 5_000_000 });
td.trackMarketData(md);                   // 把 C++ 行情快照接为偏离与金额估值参考价
td.setInstrumentPositionLimits({ rb2610: 100, ru2610: { long: 100, short: 20 } });
td.setInstrumentMarginLimits({ ag2608: 2_000_000, au2608: 5_000_000 }); // 按合约保证金上限
td.reduceOnly(); td.resume();              // 拦截增险入口，保留减仓/撤单

// 来自 C++ 最新值缓存的同步行情快照（无需等待事件）：
const tick = md.snapshot("rb2510");       // 最新的完整 DepthMarketDataField，或 null
const px = md.last("rb2510");             // 只取最新价（无则为 0）

// 乘数与保证金率在 C++ 内由 SPI 回调获取（无 JS 喂入）。一次普通查询即可保持最新——没有要回传的：
await td.reqQryInstrument({});            // 乘数 <- OnRspQryInstrument
await td.reqQryInstrumentMarginRate({ instrumentId: "rb2510" }); // 费率 <- OnRspQryInstrumentMarginRate

// 有意的、由 JS 驱动的原子重同步：
await td.syncOrders();                    // 重建预约，同时标记持仓侧不确定
await td.syncPositions();                 // 最后原子替换已持仓，保留预约

// 延迟攸关的预埋触发器（从 C++ 在 MD 线程上触发）：
const armed = td.arm(md, {
  instrumentId: "rb2510", side: "buy", triggerPrice: 3500,
  order: { /* InputOrder；orderRef 留空会自动分配 */ },
});
armed.disarm();                           // 移除触发器
// 一次性；成交/回执经普通的 rtn-order / rtn-trade 事件到达。
```

## 状态

离线测试覆盖风控账本、arm 状态机、生成发送路径、背压和生命周期隔离。另有凭据门控的 SimNow
集成测试，但不能把它表述成持续浸泡覆盖。金额公式覆盖按金额、按手以及相对表 + 交易所基础表；
元数据缺失时 fail-closed，实盘浸泡仍是独立发布门槛。

## 进程生命周期 一次创建并复用

`MarketData` 和 `Trader` 是**一次创建、进程内一直复用**的对象。包装构造函数调用原生 `_start()`，
后者调用厂商 `Init()`；`close()` 调用厂商 `Release()`（`CThostFtdcMdApi::Release()` /
`CThostFtdcTraderApi::Release()`——见 `src/native/mdapi.cc` 和 `src/native/traderapi.cc` 里的
`doClose()`）。

**已观测风险。** 在当前随包的 Windows 厂商 DLL 上，同一进程反复
`Init()`→`Release()` 会可重现地卡在 `Release()` 内部——经验上 4–11 次，随时机和 CTP 内部
重连线程状态而变。闭源厂商调用一旦卡住，本绑定无法让该进程恢复，因此当前候选版明确采用
“一次创建”生命周期。（对照：*不*调用 `_start()` 的裸原生对象能扛过 80+ 次 create/close 循环；触发点
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

行情环是 `DropPolicy::Oldest`：当 JS 消费者落后一整环（8192 条记录）于 tick 洪流时，新记录进入
有界的 keep-newest 暂存队列，并优先淘汰较旧的**可丢 tick**；它不会覆盖 JS 正在解码的字节，且会保留最新暂存 tick。请求响应
（`OnRspUserLogin` / `OnRspUserLogout` / `OnRspQryMulticastInstrument`）与流式 tick 共用这个环，
但会被标记为不可丢；即使因此暂时超过 tick 容量也会保留。JS 事件循环若长期停顿，响应仍可能延迟到
等待 Promise 的空闲超时（`requestTimeoutMs`，默认 30 秒），但通道不会主动丢弃它。

**交易端**使用 `DropPolicy::Lossless`：4096 槽满后进入有序溢出队列，再随消费者前进回填。
它不主动丢回报，但持续消费停顿会增长内存。
