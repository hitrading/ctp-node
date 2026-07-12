<!-- LANG-SWITCH -->
[English](troubleshooting.md) · **简体中文**

# 故障排查与 FAQ

常见故障及修复方法。另见 [API 参考](API.zh-CN.md) 与 [原生内部](native-hooks.zh-CN.md)。

- [连接](#连接) · [登录 / 认证](#登录--认证) · [查询与限频](#查询与限频)
- [报单被拒](#报单被拒) · [涨停 / 跌停](#涨停--跌停) · [性能与内存](#性能与内存)
- [生命周期与重连](#生命周期与重连) · [构建与安装](#构建与安装) · [错误码参考](#错误码参考)

---

## 连接

### `front-connected` 从不触发 / 收不到行情

前置地址错误或不可达。SimNow **会轮换前置**——旧的 `180.168.146.187:10201/10211` 已停用；
撰写时在用的站点是 **上期技术-电信二**：交易 `tcp://182.254.243.31:30002`、行情
`tcp://182.254.243.31:30012`。

1. 在 SimNow 客户端的 **代理及服务器配置** 里查看你当前的前置（它显示的「交易服务器地址 / 行情
   服务器地址」及实时延迟）。
2. 显式传入：`new Trader("./flow/td/", "tcp://HOST:PORT")` /
   `new MarketData("./flow/md/", "tcp://HOST:PORT")`。
3. 先确认裸 TCP 可达：
   ```js
   require("net").connect({ host: "182.254.243.31", port: 30002, timeout: 5000 })
     .on("connect", () => console.log("TCP OPEN")).on("error", (e) => console.log(e.code));
   ```
   `timeout`/`ECONNREFUSED` = 前置错误或出网被挡（VPN/防火墙），不是绑定的问题。

### 连上了但没有行情

连上 ≠ 已订阅。你必须在 `front-connected` 处理函数里先 `await md.login(...)`，再
`md.subscribe([...])`。另外：合约只有在交易时才有 tick——不在其交易时段（如只在夜盘交易的品种在
白天）时，即使订阅了也静默。

---

## 登录 / 认证

| 症状 | 原因 | 修复 |
|---|---|---|
| 登录被拒，"CTP:不合法的登录" | brokerId / userId / password 错误 | SimNow 的 brokerId 是 `9999`；核对账户。 |
| `reqAuthenticate` 失败 | appId / authCode 错误（终端认证） | SimNow：`appId: "simnow_client_test"`、`authCode: "0000000000000000"`。实盘用券商下发的值。 |
| "CTP:还没有初始化" | 在 `front-connected` 之前就发了请求 | 在 `front-connected` 处理函数里做登录/查询。 |
| 实盘登录后报单立刻被拒 | 未确认结算单 | 调 `reqSettlementInfoConfirm(...)`（或直接用 `session()`，它会做）。 |
| 登录后第一笔查询为空/被拒 | 查得太早（冷启动） | 用 `sync*` 辅助方法（它们会穿越限频重试），或等约 1 秒。 |

`session()` 会按正确顺序、带正确重试地完成 认证 → 登录 → 确认 → 同步——除非你要精细控制，否则优先
用它而非手写。

```ts
td.on("front-connected", () => td.session({
  brokerId: "9999", userId: "id", password: "pw",
  appId: "simnow_client_test", authCode: "0000000000000000",
}).catch((e) => console.error("session 失败", e.errorId, e.errorMsg)));
```

---

## 查询与限频

CTP 对每个会话的**查询限频约每秒一次**。查得太快（或登录后太早）会返回空或以限流错误被拒。

- 使用 `syncMultipliers()` / `syncOrders()` / `syncPositions()`——它们会重试瞬时 `-2`/`-3`
  限流错误，以及必须存在但冷启动时暂为空的数据。
- 原始 `reqQry*` / `reqQuery*` **不需要**调用方再写 `setTimeout(1000)`。同一个客户端内的所有查询
  在调用时即快照参数，并进入一个 FIFO 调度器：最多一个查询在途，两次尝试发送至少间隔 1100 ms；
  断线、登录或登出会拒绝旧会话中尚未发送的查询。
- 调度器按 `Trader`/`MarketData` 实例隔离，并非进程全局。若多个实例共享券商/账户级配额，仍需由
  应用网关统一协调。队列上限为 256；应 await 较早的查询，不要制造持续数小时的 `Promise.all` 堆积。
- 发送侧限频也作用于**报单**：CTP 限制发送速率。配置
  `setRiskLimits({ maxOrdersPerSec, orderBurst })`，让 C++ 令牌桶把你的发送节流到限制以下，而不是吃
  交易所拒单。
- 对应的发送返回码：`-2` = 在途未处理请求过多，`-3` = 每秒发送速率超限（见[参考](#错误码参考)）。

---

## 报单被拒

`reqOrderInsert()` **提交成功即 resolve**，仅当*发送*被拒时才 reject；被**交易所**拒绝的报单以
`err-rtn-order-insert` 事件回来（带 `options.rspInfo.errorMsg`），状态变化通过 `rtn-order` 到达。
两者都要监听。

| `errorMsg`（CTP 中文） | 含义 | 修复 |
|---|---|---|
| `不允许重复报单` | `OrderRef` 重复 / 非数字 | **不要自己传 `orderRef`**——留空，Trader 会自动分配唯一数字 ref（播种在券商 `maxOrderRef` 之上）。若必须设置，用严格递增的数字。 |
| `资金不足` | 保证金不足 | 减小手数，或平掉其他持仓；查 `reqQryTradingAccount().available`。 |
| `平仓量超过持仓量` / `可平仓位不足` | 平的比持有的多 | 核对持仓；SHFE/INE 上选对开平（平今 vs 平昨）。 |
| 平今/平昨 相关错误 | SHFE/INE 上开平标志错误 | 用 `OffsetFlag.CloseToday`（`"3"`）vs `OffsetFlag.CloseYesterday`（`"4"`），而非普通 `Close`。 |
| `报单价格超出涨跌停板` | 价格超出当日涨跌停 | 把价格夹到 ±限内；要挂着不成交的单，用离盘口几个 tick 的价格，而非很远。 |
| `报单字段有误` | `InputOrder` 字段不全 | 带上 CTP 需要的完整字段（`combHedgeFlag`、`orderPriceType`、`timeCondition`、`volumeCondition`、`contingentCondition`、`forceCloseReason`……）。见[下单示例](API.zh-CN.md#实战示例)。 |
| `CTP:撤单找不到对应报单` | 撤一笔已经没了的单 | 该单已成交/已撤；忽略即可。 |

如果是你自己的盘前风控拦的，**被 reject 的 Promise** 消息会说明
（`blocked by pre-trade risk: ...`、`rate limited`、`position ... limit`）——那是你的
`setRiskLimits`/`setInstrumentPositionLimit`/风控模式，不是交易所。

---

## 涨停 / 跌停

**跌停**时没有买盘，**涨停**时没有卖盘。CTP 用哨兵值 `DBL_MAX`（`Number.MAX_VALUE`，约 1.8e308）
填充空的那一边，**而非** 0。

- **预埋触发器**已对此防护：卖单在 `bid ≥ trigger` 时触发，绑定会忽略 `DBL_MAX` 的 bid，因此不会
  在跌停时触发恐慌性卖出（买单对 `DBL_MAX` 的 ask 同理）。
- **你自己的逻辑也必须防护**——使用 `tick.bidPrice1` / `lastPrice` 前，检查
  `px > 0 && px < 1e300`。风控引擎的参考价、成本、名义价值路径已经拒绝该哨兵值。
- **跌停时撤单可用**——`reqOrderAction` 不受价格限制，`reduceOnly()` 与 `haltAll()` 都故意保持撤单
  开放，因此限制新报单时你仍能撤回挂单。

```ts
md.on("rtn-depth-market-data", (t) => {
  if (!(t.bidPrice1 > 0) || t.bidPrice1 >= 1e300) return; // 没有买盘（跌停）
  // ……可以安全使用该报价
});
```

---

## 性能与内存

一次历史实测（不是持续浸泡覆盖）订阅了**全市场**（SimNow，18,111 个合约，并做逐 tick SMA +
定时下单/撤单）：RSS 稳定在约 124 MB，CPU 0–2%，**0 条丢弃**，策略约 3 µs/tick，事件循环延迟正常。

确切的 `3.0.0-rc.1` 候选版还提供可复现的离线探针：

```bash
npm run bench:fullmarket
```

2026-07-11 在 Node 24.18.0 / Windows 11 / Intel Core Ultra 9 285K（24 个逻辑 CPU、96 GiB 内存）上，
完整路径由原生后台线程调用真实 `MdSpi`：18,111 个不同 `InstrumentID`、原生快照锁/缓存、队列、
生成的 46 字段解码器、EventEmitter，以及通过每条真实 `tick.instrumentId` 查找的 18,111 份字符串键
20-tick 滚动状态。

- 5 万/s、5 秒：250,000/250,000 全收，0 丢弃；峰值 RSS 129 MiB；占单核 1.55%，ELU 6.1%。
- 50 万/s、30 秒：15,000,000/15,000,000 全收，0 丢弃；峰值 RSS 247 MiB；占单核 6.62%
  （整台 24 核机器的 0.28%），ELU 26.2%。该 30 秒窗口始终低于这一 RSS 峰值，但它不能单独证明
  更长时间绝无泄漏。
- 100 万/s、3 秒：交付 2,994,819，keep-newest 丢弃 5,181（0.173%）；峰值 RSS 242 MiB；
  占单核 73.8%，ELU 64.3%。这已超出实测零丢失区间。
- 一次把 18,111 个合约各 1 tick 紧密连续回调：18,111/18,111 全收、0 丢弃，约 10.9 ms 排空。

这证明该机器相对 5 万/s 参考负载至少有 10 倍的零丢失吞吐余量，不保证其他 CPU 或更重处理函数仍有
同样余量。producer 包含本 binding 的原生回调循环，但不含 CTP 网络解析。CTP 回调流本身是串行的，
所以所谓「同时」按上述紧密连续 callback storm 建模。历史实盘只能作旁证，不能代替当前候选制品浸泡；
生产验收前仍应在开盘/集合竞价对确切打包制品运行 `scripts/stress-fullmarket.mjs` 30–60 分钟。

- **`droppedRecords > 0` 且持续上涨** = 你的 JS 处理函数跟不上，行情环正在丢弃最旧的 tick
  （「行情挤压」）。把重活移出 tick 路径（批处理，或交给 worker）。交易端回报进入有序无损队列，
  长时间阻塞处理函数会增长内存而不是丢回报。
- **内存无上限增长** 通常是应用保留对象，也可能是交易端消费持续慢于回报生产。只保留有界状态，
  并让交易回报处理函数保持短小。
- 行情固定环约 21.2 MiB；有界 keep-newest tick 暂存区最多还会暂存约一环 payload。不可丢的 MD 控制
  回报不计入 tick 上限。交易固定环约 10.6 MiB。行情过载会丢最旧 tick、保留最新值，因此 tick 有界；
  交易端为不丢回报采用无界溢出区，若 JS 长时间停止消费，内存理论上可以增长到 GiB 级。
- **大的 `reqQryInstrument({})`**（约 1.8 万个合约）会流式传输约 30–50 秒并以一个数组 resolve——
  正常现象；为这段冷启动时间留预算。

---

## 生命周期与重连

**绝不要靠销毁再重建客户端来重连。** CTP 厂商 DLL 在一个进程内做几次 `Init()`/`Release()` 循环
后会在 `Release()` 内部死锁。CTP 会自行重连——在每次 `front-connected` **原地**重跑握手。`close()`
只在关闭时调一次。见 [native-hooks.zh-CN.md → 进程生命周期](native-hooks.zh-CN.md#进程生命周期-一次创建并复用)。

- 若你在一个进程内创建/关闭了过多客户端，会打印一次性的 `console.warn`（`CTP_RECREATE_WARN`）——
  请重视它。
- **Windows：进程退出时卡住 / 杀不掉**——卡死的 CTP `Release()` 会忽略 `SIGTERM`；用
  `taskkill /F /PID <pid>`。在它死掉前会一直占着 `build\Release\ctp.node`（见下方构建错误）。

---

## 构建与安装

| 症状 | 原因 | 修复 |
|---|---|---|
| `LNK1104: cannot open file ... ctp.node` | 一个卡死/僵尸 `node` 进程还占着该插件 | 找到并 `taskkill /F` 掉游离的 node 进程，再重新构建。 |
| `fatal error LNK1103: 调试信息损坏` | 陈旧/损坏的构建产物（如被中断/并行构建） | `npx node-gyp rebuild`（干净重建）。 |
| 使用方安装时 `MODULE_NOT_FOUND: node-addon-api` | 源码构建但缺构建依赖 | 已修复——`node-addon-api`/`node-gyp` 是运行时 `dependencies`；在未预编译的平台上仍需 C++ 工具链 + Python。 |
| macOS arm64 / linux arm64 上 `.node` 加载失败 | 该平台无预编译 | 安装时从源码构建；确保有工具链。 |
| 构建时 C4819 代码页警告 | GBK 代码页下 `.cc` 里的中文注释 | 无害；忽略。 |

---

## 错误码参考

**发送返回码**（`reqXxx` 回传的值；对 `reqOrderInsert` 这类提交即 resolve 的方法，非零值会让
Promise reject）：

| 码 | 含义 |
|---|---|
| `0` | 已接受 / 已发送 |
| `-1` | 网络发送失败（未连接，或 CTP 拒绝发送） |
| `-2` | 在途未处理请求过多 |
| `-3` | 每秒发送速率超限（CTP 限流） |
| `-10001` | 被本绑定的盘前风控拦截（拒单消息含具体原因） |
| `-10002` | 被已配置的开仓或平仓限频器拦截 |
| `-10003` | 被持仓**保证金**上限拦截（账户级 `maxAccountMargin`，或按合约的 `setInstrumentMarginLimit`；公式含按金额与按手部分） |
| `-10004` | 被持仓**手数**上限拦截（`setInstrumentPositionLimit`） |
| `-10005` | 同一 front/session 内的重复 `OrderRef` |
| `-10006` | 复杂报单无法穿过已配置的本地风控，已 fail-closed |
| `-10007` | 缺少必要的价格/乘数/保证金元数据，或为相对值 |
| `-10008` | 无效的报单方向或开平标志 |
| `-10009` | 行情登录/登出被拒：会话切换正在进行或状态不明（请等待其完成，或超时后重连） |

**交易所/券商错误**以 `rspInfo`（`{ errorId, errorMsg }`）出现在相应事件上，被 reject 的请求
Promise 上则是 `err.errorId` / `err.errorMsg`。`errorId 0` = 成功；非零带一个中文 `errorMsg`
（已替你 GB18030 解码）——消息是可靠、可读的原因，请记录它。常见的见上面的
[报单被拒](#报单被拒) 和 [登录](#登录--认证) 表格。

```ts
try { await td.reqUserLogin({ brokerId: "9999", userId: "id", password: "bad" }); }
catch (e) { console.error(`CTP ${e.errorId}: ${e.errorMsg}`); }
```
