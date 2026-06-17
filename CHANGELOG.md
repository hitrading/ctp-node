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
