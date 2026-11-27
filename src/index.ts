/*
 * index.ts - public entry point.
 */

export { MarketData, MarketDataEvent } from "./market-data.js";
export type { MdLoginReq } from "./market-data.js";
export { Trader, TraderEvent } from "./trader.js";
export type {
  RiskLimits,
  MarginRateSyncOptions,
  MarginRateCatalogRow,
  TraderSessionIdentity,
  TraderSessionResult,
  SessionOptions,
} from "./trader.js";
export { CtpClient } from "./client.js";
export {
  ctpInvestorGroupNeeded, ctpProductExchange, groupCtpInstrumentsByProduct, selectCtpInvestorRates, selectCtpProductRate,
} from "./product-rates.js";
export type {
  CtpInvestorRateRow, CtpProductInstrument, CtpProductRateRequest, CtpRateAccount, CtpRateRowSelection, CtpResolvedMarginRate, InvestorRateKind,
} from "./product-rates.js";
export { isTransientQueryFailure } from "./query-pacing.js";
export type { CallbackOptions, RspInfo } from "./client.js";

// Generated CTP enums (values) and struct interfaces (types).
export * from "./generated/enums.gen.js";
export type * from "./generated/structs.gen.js";
