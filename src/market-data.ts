/*
 * market-data.ts - the MarketData client (行情).
 *
 * EventEmitter for streaming pushes (rtn-depth-market-data, ...) + Promise for
 * request/response (login). Event codes mirror src/native/mdspi.h.
 */

import { native } from "./native-binding.js";
import { CtpClient, type CallbackOptions } from "./client.js";
import type {
  DepthMarketData,
  ForQuoteRsp,
  SpecificInstrument,
  RspUserLogin,
  UserLogout,
} from "./generated/structs.gen.js";

const MD_BASE = 0x1000;
const MD_EVENTS = new Map<number, string>([
  [MD_BASE + 1, "front-connected"],
  [MD_BASE + 2, "front-disconnected"],
  [MD_BASE + 3, "heart-beat-warning"],
  [MD_BASE + 4, "rsp-user-login"],
  [MD_BASE + 5, "rsp-user-logout"],
  [MD_BASE + 6, "rsp-qry-multicast-instrument"],
  [MD_BASE + 7, "rsp-error"],
  [MD_BASE + 8, "rsp-sub-market-data"],
  [MD_BASE + 9, "rsp-unsub-market-data"],
  [MD_BASE + 10, "rsp-sub-for-quote"],
  [MD_BASE + 11, "rsp-unsub-for-quote"],
  [MD_BASE + 12, "rtn-depth-market-data"],
  [MD_BASE + 13, "rtn-for-quote"],
]);

export interface MdLoginReq {
  tradingDay?: string;
  brokerId?: string;
  userId?: string;
  password?: string;
  userProductInfo?: string;
}

/** Market-data event names (symbolic; plain strings also work). */
export enum MarketDataEvent {
  FrontConnected = "front-connected",
  FrontDisconnected = "front-disconnected",
  HeartBeatWarning = "heart-beat-warning",
  RspUserLogin = "rsp-user-login",
  RspUserLogout = "rsp-user-logout",
  RspQryMulticastInstrument = "rsp-qry-multicast-instrument",
  RspError = "rsp-error",
  RspSubMarketData = "rsp-sub-market-data",
  RspUnSubMarketData = "rsp-unsub-market-data",
  RspSubForQuote = "rsp-sub-for-quote",
  RspUnSubForQuote = "rsp-unsub-for-quote",
  RtnDepthMarketData = "rtn-depth-market-data",
  RtnForQuote = "rtn-for-quote",
}

export class MarketData extends CtpClient {
  constructor(flowPath: string, fronts: string | string[]) {
    super(new native.MarketData(flowPath, fronts), native.__layoutData(), MD_EVENTS);
    // SimNow's MD OnRspUserLogin returns requestId 0; only one request (login)
    // is ever in flight here, so resolving an id-0 response against the oldest
    // pending request is safe and necessary.
    this.zeroIdFallback = true;
    this.start();
  }

  getApiVersion(): string {
    return (this.native as Record<string, () => string>).getApiVersion();
  }
  getTradingDay(): string {
    return (this.native as Record<string, () => string>).getTradingDay();
  }

  /** Subscribe to depth market data. Returns the CTP send code (0 = ok). */
  subscribe(instrumentIds: string[]): number {
    return (this.native as { subscribeMarketData(a: string[]): number }).subscribeMarketData(instrumentIds);
  }
  unsubscribe(instrumentIds: string[]): number {
    return (this.native as { unsubscribeMarketData(a: string[]): number }).unsubscribeMarketData(instrumentIds);
  }
  subscribeForQuote(instrumentIds: string[]): number {
    return (this.native as { subscribeForQuoteRsp(a: string[]): number }).subscribeForQuoteRsp(instrumentIds);
  }
  unsubscribeForQuote(instrumentIds: string[]): number {
    return (this.native as { unsubscribeForQuoteRsp(a: string[]): number }).unsubscribeForQuoteRsp(instrumentIds);
  }

  /** Log in; resolves with the login response (or rejects on CTP error). */
  login(req: MdLoginReq = {}): Promise<RspUserLogin> {
    return this.request<RspUserLogin>(
      (id) => (this.native as { reqUserLogin(r: MdLoginReq, id: number): number }).reqUserLogin(req, id),
      true
    );
  }
  logout(req: { brokerId?: string; userId?: string } = {}): Promise<UserLogout> {
    return this.request<UserLogout>(
      (id) => (this.native as { reqUserLogout(r: object, id: number): number }).reqUserLogout(req, id),
      true
    );
  }

  /** Route this market data's ticks to a Trader's armed triggers (see Trader.arm). */
  attachArm(trader: { _armRegistry(): unknown }): void {
    this.native._attachArm(trader._armRegistry());
  }

  /** @internal test-only: inject a synthetic depth tick into the ring. */
  _injectTestTick(): void {
    (this.native as { _injectTestTick(): void })._injectTestTick();
  }

  // Typed event signatures.
  on(event: "rtn-depth-market-data", cb: (data: DepthMarketData, opts: CallbackOptions) => void): this;
  on(event: "rtn-for-quote", cb: (data: ForQuoteRsp, opts: CallbackOptions) => void): this;
  on(event: "front-connected", cb: (data: undefined, opts: CallbackOptions) => void): this;
  on(event: "front-disconnected", cb: (reason: number, opts: CallbackOptions) => void): this;
  on(event: "heart-beat-warning", cb: (lapse: number, opts: CallbackOptions) => void): this;
  on(event: "rsp-user-login", cb: (data: RspUserLogin, opts: CallbackOptions) => void): this;
  on(event: "rsp-sub-market-data", cb: (data: SpecificInstrument, opts: CallbackOptions) => void): this;
  on(event: "rsp-error", cb: (data: undefined, opts: CallbackOptions) => void): this;
  on(event: string, cb: (...args: never[]) => void): this;
  on(event: string, cb: (...args: never[]) => void): this {
    return super.on(event, cb as (...args: unknown[]) => void);
  }
}
