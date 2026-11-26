# [5.0.0](https://github.com/hitrading/ctp-node/compare/v4.3.1...v5.0.0) (2026-11-26)


* feat(risk)!: 原生风控只保留无状态闸门，删除持仓委托账本与预授权单 ([61535b7](https://github.com/hitrading/ctp-node/commit/61535b75083fb44ca6fd1b88f798a83ccb725759))


### Bug Fixes

* **trader:** 会话确认前拒绝不支持的请求并写明原因，文档与测试按审核修正 ([9ef50c6](https://github.com/hitrading/ctp-node/commit/9ef50c6d46475cd9ab7cd5d22dd85cdaaa2bb3de))


### BREAKING CHANGES

* 删除 RiskLimits.maxAccountMargin、setInstrumentPositionLimit(s)、
setInstrumentMarginLimit(s)、syncPositions、syncOrders、seedPositions、seedFromPositions、
resetPositions、accountMargin、arm、armStats、MarketData.attachArm，以及 session() 的
sync.positions/sync.orders 选项与结果字段；返回码 -10003、-10004、-10005、-10007 退役。
迁移说明见 docs/MIGRATION.md。

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>

## [4.3.1](https://github.com/hitrading/ctp-node/compare/v4.3.0...v4.3.1) (2026-11-25)


### Bug Fixes

* **trader:** 快照遇新成交的重取按时间封顶，不随查询限速缩短 ([7268dfd](https://github.com/hitrading/ctp-node/commit/7268dfd02ce024e7b0a545c8c899ba01d454c3e0))

# [4.3.0](https://github.com/hitrading/ctp-node/compare/v4.2.1...v4.3.0) (2026-11-25)


### Bug Fixes

* **trader:** 按柜台应答判断就绪，空快照以资金账户核对 ([95eb37b](https://github.com/hitrading/ctp-node/commit/95eb37b18840c50203efa33e152f1deefc928beb))
* **trader:** 重试与空快照复核的等待不随查询限速缩短 ([61de6d9](https://github.com/hitrading/ctp-node/commit/61de6d9a766cd361e3d1611d16f590d9be6b9683))


### Features

* **trader:** 按前置下发的流控限速查询并提供 frontInfo ([140751c](https://github.com/hitrading/ctp-node/commit/140751c81832cd9149bf0fc169c9ec1cdcdb973c))

## [4.2.1](https://github.com/hitrading/ctp-node/compare/v4.2.0...v4.2.1) (2026-11-24)


### Bug Fixes

* **risk:** 对成交先到的终态核验实际成交手数 ([f5f8ad3](https://github.com/hitrading/ctp-node/commit/f5f8ad3842ee3962df68f213c98781bf4ea8bf05))
* **risk:** 核对成交身份与开仓委托终态凭证 ([0649ec1](https://github.com/hitrading/ctp-node/commit/0649ec1daaca390598655b0345e36cbf8752d736))
* **risk:** 核验成交数量并统一交易所回报编号 ([4bdac73](https://github.com/hitrading/ctp-node/commit/4bdac733d9783ae045953962490eeb1ec90ab754))
* **risk:** 禁止同号快照恢复已矛盾的委托终态凭证 ([c25e49c](https://github.com/hitrading/ctp-node/commit/c25e49c5ae4b6830990120e15bc263fc5adff9ff))
* **risk:** 首次终态也须核对已知委托的原始手数 ([f35c682](https://github.com/hitrading/ctp-node/commit/f35c682870f7b0959c3aa7a69661f28b17239fd0))

# [4.2.0](https://github.com/hitrading/ctp-node/compare/v4.1.3...v4.2.0) (2026-11-24)


### Features

* **risk:** report the margin breakdown at opening admission ([8c2f3f6](https://github.com/hitrading/ctp-node/commit/8c2f3f6aac4efa6014a2313d7681c95abaa902cb))

## [4.1.3](https://github.com/hitrading/ctp-node/compare/v4.1.2...v4.1.3) (2026-11-13)


### Bug Fixes

* **macos:** CTP 框架与头文件版本不一致时在构建期中止，不再编出运行时崩溃的插件 ([88de78f](https://github.com/hitrading/ctp-node/commit/88de78f956a12e43082a0bc6eb577694df96e94c))
* **macos:** 换上 CTP v6.7.13 MacOS framework，恢复 macOS 构建 ([c1a1950](https://github.com/hitrading/ctp-node/commit/c1a1950c0bd6d57fee99fa64b84c5ca41e0b0f15))

## [4.1.3-rc.2](https://github.com/hitrading/ctp-node/compare/v4.1.3-rc.1...v4.1.3-rc.2) (2026-11-13)


### Bug Fixes

* **macos:** 换上 CTP v6.7.13 MacOS framework，恢复 macOS 构建 ([c1a1950](https://github.com/hitrading/ctp-node/commit/c1a1950c0bd6d57fee99fa64b84c5ca41e0b0f15))

## [4.1.3-rc.1](https://github.com/hitrading/ctp-node/compare/v4.1.2...v4.1.3-rc.1) (2026-11-13)


### Bug Fixes

* **macos:** CTP 框架与头文件版本不一致时在构建期中止，不再编出运行时崩溃的插件 ([88de78f](https://github.com/hitrading/ctp-node/commit/88de78f956a12e43082a0bc6eb577694df96e94c))

## [4.1.2](https://github.com/hitrading/ctp-node/compare/v4.1.1...v4.1.2) (2026-11-13)


### Bug Fixes

* **native:** CTP API 推迟到 start() 创建，未启动的实例关闭时不再调用 Release() ([266a1a8](https://github.com/hitrading/ctp-node/commit/266a1a84842d489ba2b06712a11d134beb990a6a))

## [4.1.2-rc.1](https://github.com/hitrading/ctp-node/compare/v4.1.1...v4.1.2-rc.1) (2026-11-13)


### Bug Fixes

* **native:** CTP API 推迟到 start() 创建，未启动的实例关闭时不再调用 Release() ([266a1a8](https://github.com/hitrading/ctp-node/commit/266a1a84842d489ba2b06712a11d134beb990a6a))

## [4.1.1](https://github.com/hitrading/ctp-node/compare/v4.1.0...v4.1.1) (2026-11-13)


### Bug Fixes

* **build:** Linux 预编译件按库名链接 CTP 动态库，只经 $ORIGIN 解析 ([4de6bdd](https://github.com/hitrading/ctp-node/commit/4de6bdd0b921d271f56112b6ec5f773569d5bdfc))
* **build:** Linux 预编译件附带未经改动的 CTP 官方动态库 ([3b333c3](https://github.com/hitrading/ctp-node/commit/3b333c339b1799b495f6cf7cd9a5ef711a0b4f21))
* **build:** 去掉 prebuildify --strip，构建不再改动 CTP SDK 原件 ([5010d58](https://github.com/hitrading/ctp-node/commit/5010d582e5dd579efbd2ac8819269bb5d09a7f71))

## [4.1.1-rc.3](https://github.com/hitrading/ctp-node/compare/v4.1.1-rc.2...v4.1.1-rc.3) (2026-11-13)


### Bug Fixes

* **build:** 去掉 prebuildify --strip，构建不再改动 CTP SDK 原件 ([5010d58](https://github.com/hitrading/ctp-node/commit/5010d582e5dd579efbd2ac8819269bb5d09a7f71))

## [4.1.1-rc.2](https://github.com/hitrading/ctp-node/compare/v4.1.1-rc.1...v4.1.1-rc.2) (2026-11-13)


### Bug Fixes

* **build:** Linux 预编译件附带未经改动的 CTP 官方动态库 ([3b333c3](https://github.com/hitrading/ctp-node/commit/3b333c339b1799b495f6cf7cd9a5ef711a0b4f21))

## [4.1.1-rc.1](https://github.com/hitrading/ctp-node/compare/v4.1.0...v4.1.1-rc.1) (2026-11-13)


### Bug Fixes

* **build:** Linux 预编译件按库名链接 CTP 动态库，只经 $ORIGIN 解析 ([4de6bdd](https://github.com/hitrading/ctp-node/commit/4de6bdd0b921d271f56112b6ec5f773569d5bdfc))

# [4.1.0](https://github.com/hitrading/ctp-node/compare/v4.0.1...v4.1.0) (2026-10-12)


### Bug Fixes

* **trader:** 以最终合约目录统一品种归属和费率失效范围 ([f4e7147](https://github.com/hitrading/ctp-node/commit/f4e714741f0cab91c52860c7ebc3d1d1dcef967d))


### Features

* **trader:** 按品种批量同步保证金并原子应用合约覆盖 ([c52819b](https://github.com/hitrading/ctp-node/commit/c52819b932153859c71168377ca6f8d3bc6d6963))

# [4.1.0-rc.2](https://github.com/hitrading/ctp-node/compare/v4.1.0-rc.1...v4.1.0-rc.2) (2026-10-12)


### Bug Fixes

* **trader:** 以最终合约目录统一品种归属和费率失效范围 ([f4e7147](https://github.com/hitrading/ctp-node/commit/f4e714741f0cab91c52860c7ebc3d1d1dcef967d))

# [4.1.0-rc.1](https://github.com/hitrading/ctp-node/compare/v4.0.1...v4.1.0-rc.1) (2026-10-12)


### Features

* **trader:** 按品种批量同步保证金并原子应用合约覆盖 ([c52819b](https://github.com/hitrading/ctp-node/commit/c52819b932153859c71168377ca6f8d3bc6d6963))

## [4.0.1](https://github.com/hitrading/ctp-node/compare/v4.0.0...v4.0.1) (2026-09-02)


### Bug Fixes

* **trader:** accept signed non-zero session IDs ([5d7b6b0](https://github.com/hitrading/ctp-node/commit/5d7b6b0dfcb0cc399a60f57e3d475cebf7a930bf))

# [4.0.0](https://github.com/hitrading/ctp-node/compare/v3.0.0...v4.0.0) (2026-07-29)


* feat(trader)!: strengthen session sync and order policies ([7494a99](https://github.com/hitrading/ctp-node/commit/7494a9981621e50a7336c924a8a1a142ef6da515))


### Bug Fixes

* **ci:** restore coverage gate on trader changes ([00f6ae5](https://github.com/hitrading/ctp-node/commit/00f6ae5e0842a19b685c9a4beaafe153dc8bf70b))


### BREAKING CHANGES

* session.sync.marginRates now requires { instrumentIds, hedgeFlag } instead of a string array.

# [3.0.0](https://github.com/hitrading/ctp-node/compare/v2.0.0...v3.0.0) (2026-07-12)


* feat(risk)!: real-margin risk model with in-C++ data sourcing and MD snapshot LVC ([64436d5](https://github.com/hitrading/ctp-node/commit/64436d524f5c316d3da3ac85e7ded723b2d7677a))


### BREAKING CHANGES

* RiskConfig -> RiskLimits; maxPositionCost/maxMargin ->
maxAccountMargin (now real margin, x multiplier); halt() -> haltAll() +
reduceOnly(); setMaxPosition*/setMaxInstrumentCost -> setInstrumentPositionLimit(s)
/ setInstrumentMarginLimit(s); positionCost() -> accountMargin getter;
getApiVersion()/getTradingDay() -> apiVersion/tradingDay getters; seedPosition
-> seedPositions; net-mode (PosiDirection "1") positions are rejected; explicit
OrderRef must be a monotonic decimal; Node engines narrowed. See docs/MIGRATION.md.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>

# [2.0.0](https://github.com/hitrading/ctp-node/compare/v1.2.0...v2.0.0) (2026-06-17)


### Features

* **risk:** source CTP data in C++, real-margin cap, market-data snapshot cache ([474a4dc](https://github.com/hitrading/ctp-node/commit/474a4dcdee977597f621ed7fee1fe8265772fe44))


### BREAKING CHANGES

* **risk:** Trader.setMultiplier and Trader.setRefPrice are removed --
contract multipliers and the price-deviation reference are now sourced in C++
(multipliers from instrument queries, the reference via trackMarketData(md)). The
position cost cap is now margin-based; prefer riskSet({ maxMargin }).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>

# [1.2.0](https://github.com/hitrading/ctp-node/compare/v1.1.0...v1.2.0) (2026-06-16)


### Features

* upgrade CTP SDK 6.7.2 -> 6.7.13 ([bc6812a](https://github.com/hitrading/ctp-node/commit/bc6812a9d37d86b6f34cabd0dd9bf399f7fb9d45))

# [1.1.0](https://github.com/hitrading/ctp-node/compare/v1.0.0...v1.1.0) (2026-06-16)


### Features

* dual ESM + CommonJS build so require() works alongside import ([addeee4](https://github.com/hitrading/ctp-node/commit/addeee4db3d2e565b15347fd33f67749633038f4))

# 1.0.0 (2026-06-16)


### Features

* first public release of @hitrading/ctp-node ([6064b58](https://github.com/hitrading/ctp-node/commit/6064b58d5cd0455b2df0f3ddc65a555bd39d2331))

# Changelog

All notable changes to **@hitrading/ctp-node** are documented here.

This file is generated and maintained automatically by
[semantic-release](https://semantic-release.gitbook.io/) from the
[Conventional Commits](https://www.conventionalcommits.org/) history — **do not
edit it by hand**. Released versions are appended below on each release; see
[RELEASING.md](RELEASING.md) for how releases work.

<!-- semantic-release inserts released versions below this line -->
