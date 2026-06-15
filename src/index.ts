/*
 * index.ts - public entry point.
 */

export { MarketData, MarketDataEvent } from "./market-data.js";
export type { MdLoginReq } from "./market-data.js";
export { Trader, TraderEvent } from "./trader.js";
export type { RiskConfig } from "./trader.js";
export { CtpClient } from "./client.js";
export type { CallbackOptions, RspInfo } from "./client.js";

// Generated CTP enums (values) and struct interfaces (types).
export * from "./generated/enums.gen.js";
export type * from "./generated/structs.gen.js";
