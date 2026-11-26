/*
 * index.ts - public entry point.
 */

export { MarketData, MarketDataEvent } from "./market-data.js";
export type { MdLoginReq } from "./market-data.js";
export { Trader, TraderEvent } from "./trader.js";
export type {
  RiskLimits,
  MarginRateSyncOptions,
  TraderSessionIdentity,
  TraderSessionResult,
  SessionOptions,
} from "./trader.js";
export { CtpClient } from "./client.js";
export { groupCtpInstrumentsByProduct, selectCtpProductRate } from "./product-rates.js";
export type { CtpProductInstrument, CtpResolvedMarginRate } from "./product-rates.js";
export { isTransientQueryFailure } from "./query-pacing.js";
export type { CallbackOptions, RspInfo } from "./client.js";

// Generated CTP enums (values) and struct interfaces (types).
export * from "./generated/enums.gen.js";
export type * from "./generated/structs.gen.js";
