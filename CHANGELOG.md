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
