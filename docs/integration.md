# Integration tests (live SimNow) · 集成测试（实盘 SimNow）

Unit tests cover the **logic** (JS ≥ 95 %, C++ `risk`/`arm` ≥ 98 %, run on every
push). These cover the **wire behavior** against the real CTP API — what only a
live connection exposes: the connect/login handshake, the one-call
[`session()`](API.md#session) (authenticate → login → confirm settlement → sync
multipliers/positions/orders), an account query (req/rsp correlation over the
wire), and the **C++ pre-trade risk gate end-to-end**. They are
**credential-gated** and never run on push.

单元测试覆盖**逻辑**（JS ≥ 95 %、C++ `risk`/`arm` ≥ 98 %，每次 push 都跑）。这套覆盖对真实
CTP API 的**线上行为**——只有真连接才暴露的东西：连接/登录握手、一站式
[`session()`](API.zh-CN.md) 握手、账户查询（线上的请求/响应关联）、以及 **C++ 盘前风控网关的
端到端验证**。**需要凭据**，不在每次 push 上跑。

> It only ever sends **oversized** orders — the risk gate rejects them before they
> reach the exchange, so **nothing ever rests** on the account.
>
> 它只发**超量**订单——会被风控网关在送达交易所前拒掉，所以**账户上不会留任何挂单**。

## Run locally · 本地运行

```sh
CTP_USER=<investorId> CTP_PASS=<password> npm run test:integration
```

Without `CTP_USER` / `CTP_PASS` it **skips** (exit 0). Optional environment:

| env | default | meaning |
|---|---|---|
| `CTP_BROKER` | `9999` | broker id |
| `CTP_APPID` | `simnow_client_test` | terminal app id |
| `CTP_AUTHCODE` | `0000000000000000` | terminal auth code |
| `CTP_TD_FRONT` | 电信二 trade front | trade front; set a **7×24** SimNow front for off-session runs |

Credentials are read **only** from the environment — never written to a file or
committed. 凭据只从环境变量读取，绝不写入文件或提交。

## CI

[`.github/workflows/integration.yml`](../.github/workflows/integration.yml) runs
it **manually** (`workflow_dispatch`) and on a **weekday schedule** (09:30
Beijing, inside the morning session). To enable it, add the repository **secrets**
`CTP_USER` and `CTP_PASS` (Settings → Secrets and variables → Actions); optionally
set the repository **variable** `CTP_TD_FRONT`. Fork pull requests never receive
the secrets, and a run without them skips green.

手动 + 工作日定时（北京 09:30，早盘时段）运行。加仓库 **secret** `CTP_USER`/`CTP_PASS`
即启用；前置可用仓库**变量** `CTP_TD_FRONT` 覆盖。fork PR 拿不到 secret，没凭据的运行会
直接跳过（绿）。
