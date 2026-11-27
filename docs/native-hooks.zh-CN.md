<!-- LANG-SWITCH -->
[English](native-hooks.md) · **简体中文**

# 原生可下沉钩子（风控 / 限频 / 行情快照）

目标：把约 95% 的策略工作留在 JS/TS（迭代快、生态好），同时给**安全攸关**的那几片 C++ 级确定性——
任何 GC 停顿或事件循环卡顿都打不垮它们。JS 定规则；C++ 在报单路径上执行。

## 两个世界

| | C++ 世界 | JS 世界 |
|---|---|---|
| 运行于 | CTP 回调线程 | Node 主线程（事件循环） |
| GC 停顿 | 无 | 有（偶发，毫秒到几十毫秒） |
| 确定性 | 微秒级、可预测 | 抖动（GC / JIT / 调度） |

一个 tick 首先到达 **C++ 回调线程**；纯 JS 的检查必须跨到 JS 并等待事件循环。“下沉”把最后一道检查
留在 C++、紧贴厂商发送调用之前；JS 只做配置，并被告知报单为何被拒。

## 下沉了什么

1. **风控（盘前，无状态）** — `src/native/risk.h` `RiskEngine`
   每次报单发送前都跑硬性检查：熔断模式（`REDUCE_ONLY` / `HALT_ALL`）、单笔最大手数、单笔最大名义价值
   （防胖手指）以及价格偏离防护。在 C++ 里执行，因此即使 JS 进程正在 GC、被阻塞或有 bug，它们依然成立。
   *安全收益——即便慢策略也值得。*
   - **按设计无状态。** 每条规则只依据这一笔报单本身、最新行情参考价和静态合约元数据判断。没有任何
     规则依赖账户持有什么或挂着什么，因此重连后没有持仓/委托账本需要重建，也没有会与调用方账本不一致的
     状态。手数、偏离与名义价值检查作用于开仓报单；平仓报单只会被 `HALT_ALL`（或平仓限频器）拦截，
     因此缺少行情或元数据时退出仍然可用。
   - **有意不做的事：账户级上限。** 账户保证金、单合约保证金和持仓手数上限需要账户的持仓、挂单和资金。
     CTP 的持仓快照与实时报单/成交流之间没有共同序号，绑定内部的账本只能在平仓后把自己标为不确定、
     拒绝开仓，并靠资金核对去猜空快照——既重复调用方自己的账本，又可能与之冲突。这些上限属于掌握
     账户事实的交易层（`reqQryInvestorPosition`、`reqQryOrder`、`reqQryTradingAccount`、
     `rtn-order` / `rtn-trade`）；`syncMarginRates()` / `readMarginRates()` 为它的计算解析保证金率。
   - 价格偏离与名义价值检查的参考价来自 C++ 行情快照缓存（LVC；见下文 §3），经 `trackMarketData(md)`
     读取——无需 `setRefPrice`、无 JS 往返。行情缺失或超过 `maxReferenceAgeMs` 时，已启用的检查对
     开仓报单 fail-closed。名义价值按 `max(委托价, 参考价) × 量 × 乘数` 计算，只对期货/TAS 产品类启用；
     启用时其他产品类别会 fail-closed。
   - **数据来源：元数据和会话身份直接从 SPI 回调进入 C++。** 在
     `scripts/codegen/generate-trader.mjs` 中特判（生成进 `traderspi.gen.cc` / `traderreq.gen.cc`）：
     - 合约乘数与产品类别 ← `OnRspQryInstrument`；发送 `reqQryInstrument` 前会先使所查合约（不带任何
       过滤条件时为全部合约）失效；只按交易所、品种或交易所合约代码过滤的查询不使任何合约失效、发布时按合并处理，不会清掉
       范围外的合约。随后为该请求号开启暂存区。各行先暂存，只有该查询无错误的末包才一起发布。查询过滤条件
       （InstrumentID、ExchangeID、ProductID、ExchangeInstID）之外的行不会暂存；同一合约的多行在交易所、产品类别
       或乘数上不一致（或范围外出现同一合约代码的行）时，该合约不发布乘数，并标记为身份不明（无论该查询以何种方式结束；记入标记即立即作废此前已发布的乘数），直到不带交易所、
       品种、交易所合约代码过滤的查询成功、可读且带回该合约一致的行（失败、空应答或含不可读行的应答都不解除）。发送被拒、错误末包、该请求号的通用 `OnRspError`、JS 侧空闲超时、
       JS 侧任何撤销全部在途请求的情形（前置连接/断开、`session()` 开始、主动登录/登出）或更新的合约查询都会
       丢弃暂存行，因此失败、超时、残缺或被取代的刷新不会留下陈旧或残缺的元数据（末包已执行之后的撤销不会回滚
       已发布的结果）。有一行身份字段无法读取时，即便是过滤查询，成功后也会清空全部合约（失败即拒绝）。
       `syncMultipliers()` 与 `instrumentPublication(rows)` 报告的即每个请求实际发布的结果（哪些合约、各自的乘数），在该请求结算时按请求号记录（`_instrumentQueryPublication`）
     - 登录会话（FrontID/SessionID/BrokerID）← `OnRspUserLogin`，按最近一次登录请求的 request id
       关联；随后 `session()` 在结算单确认之后确认交易账户（InvestorID），之后普通报单才会被接受。前置连接/断开以及每次
       登录/登出请求都会撤销该确认。

     因此任何 `reqQryInstrument` 都会保持乘数最新——无需 JS 转交元数据。保证金率不喂给原生闸门。
   - **不受支持的请求。** 高级变更（quote/exec/comb/parked 报单、改单、对冲设置、套利/套保确认）与
     银期转账无法用这些逐笔规则判断，只在 `NORMAL`、会话已确认且未配置任何本地风控时放行，且不留下
     任何状态。每个非查询 SDK 请求都必须由生成器分类；未审核的方法会让代码生成失败。

2. **限频** — `src/native/risk.h` `RateLimiter`
   令牌桶——开仓一个、平仓另一个独立的——保证席位永不超过配置的报单速率，对停顿后的 JS 突发免疫。
   - 状态：**实**（令牌桶）。

3. **行情快照（最新值缓存）** — `src/native/snapshot.h` `SnapshotCache`
   一个 C++ 最新值缓存（LVC），按合约保存最新的完整 `DepthMarketDataField`。成功 MD 登录会激活一个
   全新的空 epoch；只有当前认证 epoch 后续的 tick 才会在发布前更新它。连接/断线、登录/登出转换和
   关闭都会使缓存立即失效，因此重连后旧报价不可能伪装成新鲜风控参考价。退订按合约清除（`erase`）。
   - **JS 同步读取**：`md.snapshot(id)` 返回最新的完整 tick（非活动 epoch、从未见过或已退订则为 `null`），
     `md.last(id)` 只返回最新价——无需等待 tick，策略里也无需逐 tick 记账。
   - **参考价完全在 C++ 内由 MD→风控流转。** Trader 的价格偏离与名义价值检查经 `trackMarketData(md)`
     在 C++ 里读取该缓存：MarketData 以 `shared_ptr` 共享该缓存，故风控引擎直接从 MD 馈源读取实时价，
     无 JS 往返。未认证或断线后的 tick 仍作为协议事件可见，但不能更新缓存。
     （`SnapshotCache` 实现了 `RefPriceSource`；风控引擎经 `setMdSnapshot` 持有它。）

## 线程 / 安全模型

- 配置写在**慢路径**（JS 线程），读在**热路径**（报单发送）。风控配置用**原子量**（无锁读取）；
  限频器用一个极小的互斥锁（有界、无争用）。
- 配置、模式切换、会话切换与每笔报单共用同一个发送屏障，因此限制/模式/会话的变更与在途发送线性一致：
  不会有报单按旧状态通过检查、却在变更返回后才发出，或以新登录的账户发出。
- 会写队列或改变测试状态的钩子（`_injectTestTick`、`_injectTestForQuote`、
  `_injectTestLifecycle`、`_injectTestLogin`、`_injectTestInstrumentRow`、`_injectTestRspError`、
  `_burstTicks`、`_produceTestTicks`、`_burstEvents`）默认禁用，只有显式设置
  `CTP_NODE_TEST_HOOKS=1` 才可调用。实盘进程绝不能开启：EventChannel 依赖厂商回调流是唯一生产者，
  而生命周期注入还会刻意绕过真实前置。

## 日志 / 可观测性

刻意地，C++ 端**不做同步日志**——在 CTP 回调线程上做阻塞 I/O 或字符串格式化，正好会引入本设计
要避免的延迟/抖动。热路径只保留 memcpy + 原子量。可观测性按低延迟系统的惯常做法分层：

- **C++ 里用计数器/事件，而非日志。** `droppedRecords`（行情安全淘汰最旧逻辑记录；交易端有序无损
  溢出）以及流式事件（`front-connected`、带原因的 `front-disconnected`、带 `errorId`/`errorMsg` 的
  `rsp-*`）都暴露给 JS。
- **原因被上抛而非吞掉。** 盘前风控拒单会把具体原因带回 JS（如 `blocked by pre-trade risk:
  order volume exceeds maxOrderVolume`；不受支持请求闸门同样会指明是风控模式、会话未确认还是配置了本地风控，限频有自己的消息），使日志能说清*为什么*。
  在热路径上零成本——它只在（罕见的）拒单时设置。
- **真正的日志在 JS 里做**，离开回调线程，从那些事件出发——任何异步日志库（pino/winston）都行。
  若将来确需 C++ 端审计日志，加一个可选的异步/延迟日志器（无锁队列 + 后台线程，类似
  NanoLog/Quill）——绝不要同步的。

## 公开的 TS 接口（外壳）

这些全部暴露在 `Trader` 上（`src/trader.ts`）：

```ts
// 无状态盘前风控（发布给 C++ 执行器；立即生效）：
td.setRiskLimits({ maxOrderVolume: 5, maxPriceDeviation: 0.02, maxNotional: 2_000_000, maxOrdersPerSec: 20 });
td.trackMarketData(md);                   // 把 C++ 行情快照接为偏离与名义价值参考价
td.reduceOnly(); td.resume();              // 拦截增险入口，保留减仓/撤单

// 来自 C++ 最新值缓存的同步行情快照（无需等待事件）：
const tick = md.snapshot("rb2510");       // 最新的完整 DepthMarketDataField，或 null
const px = md.last("rb2510");             // 只取最新价（无则为 0）

// 乘数在 C++ 内由 SPI 回调获取（无 JS 喂入）。一次普通查询即可保持最新——没有要回传的：
await td.reqQryInstrument({});            // 乘数 <- OnRspQryInstrument

// 账户事实与账户级上限放在你自己的交易层：
const positions = await td.reqQryInvestorPosition({ brokerId, investorId });
const orders = await td.reqQryOrder({ brokerId, investorId });
await td.syncMarginRates(["rb2510"], "1");
const [rate] = td.readMarginRates(["rb2510"], "1"); // 解析好的保证金率，供你自己计算保证金
```

## 状态

离线测试覆盖无状态风控闸门（独立 C++ 单元测试）、生成发送路径、背压和生命周期隔离。另有凭据门控的
SimNow 集成测试，但不能把它表述成持续浸泡覆盖；实盘浸泡仍是独立发布门槛。

## 进程生命周期 一次创建并复用

`MarketData` 和 `Trader` 是**一次创建、进程内一直复用**的对象。包装构造函数调用原生 `_start()`，
后者创建厂商 API（`CreateFtdcMdApi` / `CreateFtdcTraderApi`）并立即调用其 `Init()`；`close()` 调用
厂商 `Release()`（`CThostFtdcMdApi::Release()` / `CThostFtdcTraderApi::Release()`——见
`src/native/mdapi.cc` 和 `src/native/traderapi.cc` 里的 `doClose()`）。

**绝不对未 `Init()` 的 API 调用 `Release()`。** 厂商 `Release()` 会 join 一个只有 `Init()` 才启动的
内部反应器线程；对从未初始化的实例，它会走未定义的销毁路径，在 Ubuntu 26.04（glibc 2.43）上段错误。
因此原生对象只在 `_start()` 里、紧挨 `Init()` 之前创建厂商 API：未经 `_start()` 就被关闭或回收的
裸原生对象不持有任何厂商对象，其 `close()` 不释放任何东西。

**已观测风险。** 在当前随包的 Windows 厂商 DLL 上，同一进程反复
`Init()`→`Release()` 会可重现地卡在 `Release()` 内部——经验上 4–11 次，随时机和 CTP 内部
重连线程状态而变。闭源厂商调用一旦卡住，本绑定无法让该进程恢复，因此当前候选版明确采用
“一次创建”生命周期。（对照：*不*调用 `_start()` 的裸原生对象能扛过任意次 create/close 循环，因为它根本不创建厂商 API；触发点
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
