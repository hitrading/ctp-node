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

- 用 `syncMultipliers()` / `syncPositions()` / `syncOrders()`——它们内置了带退避的重试。
- 原始 `reqQry*` 调用之间间隔 ≥ 约 1.1 秒。
- 发送侧限频也作用于**报单**：CTP 限制发送速率。配置
  `riskSet({ maxOrdersPerSec, orderBurst })`，让 C++ 令牌桶把你的发送节流到限制以下，而不是吃
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
`riskSet`/`setMaxPosition`/`halt`，不是交易所。

---

## 涨停 / 跌停

**跌停**时没有买盘，**涨停**时没有卖盘。CTP 用哨兵值 `DBL_MAX`（`Number.MAX_VALUE`，约 1.8e308）
填充空的那一边，**而非** 0。

- **预埋触发器**已对此防护：卖单在 `bid ≥ trigger` 时触发，绑定会忽略 `DBL_MAX` 的 bid，因此不会
  在跌停时触发恐慌性卖出（买单对 `DBL_MAX` 的 ask 同理）。
- **你自己的逻辑也必须防护**——使用 `tick.bidPrice1` / `lastPrice` 前，检查
  `px > 0 && px < 1e300`。风控引擎的参考价、成本、名义价值路径已经拒绝该哨兵值。
- **跌停时撤单可用**——`reqOrderAction` 不受价格限制，且一键熔断（`halt()`）故意保持撤单开放，因此
  即使熔断时你也总能撤回挂单。

```ts
md.on("rtn-depth-market-data", (t) => {
  if (!(t.bidPrice1 > 0) || t.bidPrice1 >= 1e300) return; // 没有买盘（跌停）
  // ……可以安全使用该报价
});
```

---

## 性能与内存

订阅**全市场**没问题。实测（SimNow，18,111 个合约，对每一个做逐 tick SMA + 定时下单/撤单）：RSS
稳定在约 124 MB，CPU 0–2%，**0 条丢弃**，策略约 3 µs/tick，事件循环延迟正常。解码器可持续约
800 万 tick/s，故全市场行情有很大余量。

- **`droppedRecords > 0` 且持续上涨** = 你的 JS 处理函数跟不上，行情环正在丢弃最旧的 tick
  （「行情挤压」）。把重活移出 tick 路径（批处理，或交给 worker）；交易端的环是可靠的（丢最新），
  故报单/成交回报永不丢弃。
- **内存无上限增长** = 你在保留对象（如把每个 tick 永远 push 进数组）。绑定本身把数据解码成临时
  对象 + 固定大小的环，不会泄漏。只保留有界的按合约状态。
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
| `-10001` | 被本绑定的盘前风控拦截（`lastRiskReason()` / 消息含原因） |
| `-10002` | 被限频器拦截（`maxOrdersPerSec`） |
| `-10003` | 被持仓**成本**上限拦截（`maxPositionCost` / `setMaxPositionCost`） |
| `-10004` | 被持仓**手数**上限拦截（`setMaxPosition`） |

**交易所/券商错误**以 `rspInfo`（`{ errorId, errorMsg }`）出现在相应事件上，被 reject 的请求
Promise 上则是 `err.errorId` / `err.errorMsg`。`errorId 0` = 成功；非零带一个中文 `errorMsg`
（已替你 GB18030 解码）——消息是可靠、可读的原因，请记录它。常见的见上面的
[报单被拒](#报单被拒) 和 [登录](#登录--认证) 表格。

```ts
try { await td.reqUserLogin({ brokerId: "9999", userId: "id", password: "bad" }); }
catch (e) { console.error(`CTP ${e.errorId}: ${e.errorMsg}`); }
```
