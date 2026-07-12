/*
 * market-data.ts - the MarketData client (行情).
 *
 * EventEmitter for streaming pushes (rtn-depth-market-data, ...) + Promise for
 * request/response (login). Event codes mirror src/native/mdspi.h.
 */

import { native } from "./native-binding.js";
import { CtpClient, type CallbackOptions } from "./client.js";
import { Trader } from "./trader.js";
import type {
  DepthMarketData,
  ForQuoteRsp,
  SpecificInstrument,
  QryMulticastInstrument,
  MulticastInstrument,
  ReqUserLogin,
  RspUserLogin,
  UserLogout,
} from "./generated/structs.gen.js";
import { STRUCT_ID } from "./generated/structs.gen.js";
import {
  copyDenseStringArray,
  copyOwnFronts,
  copyOwnRecord,
  ownStringKeys,
} from "./validation.js";

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

export type MdLoginReq = Partial<ReqUserLogin>;

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

function snapshotInstrumentIds(instrumentIds: string[]): string[] {
  return copyDenseStringArray(instrumentIds, "instrumentIds");
}

function requestShapeError(
  value: unknown,
  allowed: ReadonlySet<string>,
  label: string
): TypeError | undefined {
  let keys: string[];
  try {
    keys = ownStringKeys(value, label);
  } catch (error) {
    return error instanceof TypeError ? error : new TypeError(`${label} must be a plain object`);
  }
  for (const key of keys)
    if (!allowed.has(key)) return new TypeError(`${label}: unknown field ${key}`);
  return undefined;
}

const LOGIN_KEYS = new Set([
  "tradingDay", "brokerId", "userId", "password", "userProductInfo",
  "interfaceProductInfo", "protocolInfo", "macAddress", "oneTimePassword",
  "loginRemark", "clientIpPort", "clientIpAddress", "smsCode",
]);
const LOGOUT_KEYS = new Set(["brokerId", "userId"]);
const MULTICAST_QUERY_KEYS = new Set(["topicId", "instrumentId"]);

export class MarketData extends CtpClient {
  private sessionTransition?: Promise<unknown>;

  constructor(flowPath: string, fronts: string | string[]) {
    super(
      new native.MarketData(flowPath, copyOwnFronts(fronts, "MarketData fronts")),
      native.__layoutData(),
      MD_EVENTS
    );
    this.start();
  }

  get apiVersion(): string {
    return (this.native as Record<string, () => string>).apiVersion();
  }
  get tradingDay(): string {
    return (this.native as Record<string, () => string>).tradingDay();
  }

  /** Subscribe to depth market data. Returns the CTP send code (0 = ok). */
  subscribe(instrumentIds: string[]): number {
    const ids = snapshotInstrumentIds(instrumentIds);
    return (this.native as { subscribeMarketData(a: string[]): number }).subscribeMarketData(ids);
  }
  unsubscribe(instrumentIds: string[]): number {
    const ids = snapshotInstrumentIds(instrumentIds);
    return (this.native as { unsubscribeMarketData(a: string[]): number }).unsubscribeMarketData(ids);
  }
  subscribeForQuote(instrumentIds: string[]): number {
    const ids = snapshotInstrumentIds(instrumentIds);
    return (this.native as { subscribeForQuoteRsp(a: string[]): number }).subscribeForQuoteRsp(ids);
  }
  unsubscribeForQuote(instrumentIds: string[]): number {
    const ids = snapshotInstrumentIds(instrumentIds);
    return (this.native as { unsubscribeForQuoteRsp(a: string[]): number }).unsubscribeForQuoteRsp(ids);
  }

  /** Latest cached depth tick for an instrument — the C++ last-value cache,
   *  updated on every authenticated-session tick before it reaches JS. Returns
   *  null before login, after disconnect/logout, if none has been seen, or if it
   *  was unsubscribed. Synchronous: no waiting for an event. */
  snapshot(instrumentId: string): DepthMarketData | null {
    const bytes = (this.native as { snapshot(id: string): Uint8Array | null }).snapshot(instrumentId);
    return bytes
      ? (this.decodeStruct(STRUCT_ID.DepthMarketData, bytes) as unknown as DepthMarketData)
      : null;
  }

  /** Latest price for an instrument from the cache (0 if none seen). Synchronous. */
  last(instrumentId: string): number {
    return (this.native as { last(id: string): number }).last(instrumentId);
  }

  /** @internal Hand the C++ snapshot cache to a Trader (Trader.trackMarketData),
   *  so the Trader's deviation check reads live prices in C++ — no JS round-trip. */
  _snapshotExternal(): unknown {
    return (this.native as { _snapshotExternal(): unknown })._snapshotExternal();
  }

  /** Log in; resolves with the login response (or rejects on CTP error). */
  login(req: MdLoginReq = {}): Promise<RspUserLogin | undefined> {
    const shapeError = requestShapeError(req, LOGIN_KEYS, "login request");
    if (shapeError) return Promise.reject(shapeError);
    const clean = copyOwnRecord(req, "login request");
    let encoded: Uint8Array;
    try { encoded = this.encode(STRUCT_ID.ReqUserLogin, clean); }
    catch (error) { return Promise.reject(error); }
    return this.runSessionTransition(() => {
      this.beginRequestContextTransition("market-data login started");
      return this.request<RspUserLogin | undefined>(
        (id) => (this.native as { reqUserLogin(r: Uint8Array, id: number): number }).reqUserLogin(encoded, id),
        true,
        MD_BASE + 4,
        true
      );
    });
  }
  logout(req: { brokerId?: string; userId?: string } = {}): Promise<UserLogout | undefined> {
    const shapeError = requestShapeError(req, LOGOUT_KEYS, "logout request");
    if (shapeError) return Promise.reject(shapeError);
    const clean = copyOwnRecord(req, "logout request");
    let encoded: Uint8Array;
    try { encoded = this.encode(STRUCT_ID.UserLogout, clean); }
    catch (error) { return Promise.reject(error); }
    return this.runSessionTransition(() => {
      this.beginRequestContextTransition("market-data logout started");
      return this.request<UserLogout | undefined>(
        (id) => (this.native as { reqUserLogout(r: Uint8Array, id: number): number }).reqUserLogout(encoded, id),
        true,
        MD_BASE + 5,
        true
      );
    });
  }

  protected override shouldTimeoutRequest(
    requestId: number,
    sessionTransition: boolean
  ): boolean {
    if (!sessionTransition) return true;
    return (this.native as { _timeoutSessionTransition(id: number): boolean })
      ._timeoutSessionTransition(requestId);
  }

  // MarketData confirms a successful login natively: OnRspUserLogin activates the
  // snapshot cache on the MD channel, which legitimately carries no
  // FrontID/SessionID (those are the trade front's order-routing identity). There
  // is deliberately no onRequestResolved override here — the base no-op is
  // correct, and a successful MD login must never be rejected for lacking a trade
  // session id.

  /** SimNow may return requestId=0 for MD login/logout. Never overlap these
   * transitions: otherwise a late id-0 login response cannot be attributed to
   * one epoch. If a request times out while native still considers it pending,
   * native stays fail-closed until the next front transition. */
  private runSessionTransition<T>(start: () => Promise<T>): Promise<T> {
    if (this.sessionTransition) {
      return Promise.reject(new Error(
        "market-data login/logout transition already in flight; await it first"
      ));
    }
    const promise = start();
    this.sessionTransition = promise;
    const finish = () => {
      if (this.sessionTransition !== promise) return;
      try {
        (this.native as { _abandonSessionTransition(): void })
          ._abandonSessionTransition();
      } catch {
        // close() may have torn the native object down between settlement and
        // this microtask; the native close path already invalidated the epoch.
      } finally {
        this.sessionTransition = undefined;
      }
    };
    void promise.then(finish, finish);
    return promise;
  }

  /** Query multicast instruments. Multi-row response, serialized through this
   *  client's conservative query scheduler. */
  reqQryMulticastInstrument(
    req: Partial<QryMulticastInstrument> = {}
  ): Promise<MulticastInstrument[]> {
    const shapeError = requestShapeError(req, MULTICAST_QUERY_KEYS, "multicast query");
    if (shapeError) return Promise.reject(shapeError);
    const clean = copyOwnRecord(req, "multicast query");
    return this.queryRequest<MulticastInstrument[]>((id) =>
      (this.native as {
        reqQryMulticastInstrument(r: Partial<QryMulticastInstrument>, id: number): number;
      }).reqQryMulticastInstrument(clean, id)
    );
  }

  /** Route this market data's ticks to a Trader's armed triggers (see Trader.arm).
   *  A MarketData feeds exactly one live Trader's registry. Native ownership
   *  rejects replacement while that Trader is live, but permits replacing a
   *  registry whose Trader has already closed. */
  attachArm(trader: Trader): void {
    if (!(trader instanceof Trader))
      throw new TypeError("attachArm requires a Trader from this package instance");
    // Resolve and attach first. Native shared ownership is authoritative and
    // avoids retaining a closed Trader JS object for the MarketData lifetime.
    const registry = trader._armRegistry();
    this.native._attachArm(registry);
  }

  /** @internal test-only: inject a synthetic depth tick into the ring. Optional
   *  bid/ask override the defaults (3499/3501) so tests can drive edge prices
   *  (e.g. a DBL_MAX no-bid sentinel) through the real arm/onTick path. */
  _injectTestTick(bid?: number, ask?: number): void {
    (this.native as { _injectTestTick(b?: number, a?: number): void })._injectTestTick(bid, ask);
  }

  /** @internal test-only: inject one RFQ business notification through MdSpi. */
  _injectTestForQuote(): void {
    (this.native as { _injectTestForQuote(): void })._injectTestForQuote();
  }

  /** @internal test-only: drive a real native MD lifecycle callback. */
  _injectTestLifecycle(
    state: "connected" | "disconnected" | "login" | "login-error" | "logout" |
      "login-pending" | "abandon" | "late-login"
  ): void {
    (this.native as { _injectTestLifecycle(s: string): void })._injectTestLifecycle(state);
  }

  /** @internal test-only: spawn a background producer thread that pushes `n`
   *  synthetic ticks into the ring (cross-thread doorbell stress). Each tick's
   *  requestId carries its sequence number. */
  _burstTicks(n: number, jitterUs = 0, injectControl = false): void {
    (this.native as { _burstTicks(n: number, j: number, c: boolean): void })
      ._burstTicks(n, jitterUs, injectControl);
  }

  /** @internal test-only: produce ticks on a native background thread through
   *  the real MD SPI. `ratePerSecond=0` is an instantaneous burst; otherwise
   *  ticks arrive as paced `burstSize` batches. Instrument IDs rotate across
   *  SIM00000000.. and `volume` is the zero-based global sequence. */
  _produceTestTicks(
    count: number,
    instrumentCount: number,
    ratePerSecond = 0,
    burstSize = 512
  ): void {
    (this.native as {
      _produceTestTicks(n: number, instruments: number, rate: number, burst: number): void;
    })._produceTestTicks(count, instrumentCount, ratePerSecond, burstSize);
  }

  // Typed event signatures.
  on(event: "rtn-depth-market-data", cb: (data: DepthMarketData, opts: CallbackOptions) => void): this;
  on(event: "rtn-for-quote", cb: (data: ForQuoteRsp, opts: CallbackOptions) => void): this;
  on(event: "front-connected", cb: (data: undefined, opts: CallbackOptions) => void): this;
  on(event: "front-disconnected", cb: (reason: number, opts: CallbackOptions) => void): this;
  on(event: "heart-beat-warning", cb: (lapse: number, opts: CallbackOptions) => void): this;
  on(event: "rsp-user-login", cb: (data: RspUserLogin | undefined, opts: CallbackOptions) => void): this;
  on(event: "rsp-user-logout", cb: (data: UserLogout | undefined, opts: CallbackOptions) => void): this;
  on(event: "rsp-sub-market-data", cb: (data: SpecificInstrument | undefined, opts: CallbackOptions) => void): this;
  on(event: "rsp-unsub-market-data", cb: (data: SpecificInstrument | undefined, opts: CallbackOptions) => void): this;
  on(event: "rsp-sub-for-quote", cb: (data: SpecificInstrument | undefined, opts: CallbackOptions) => void): this;
  on(event: "rsp-unsub-for-quote", cb: (data: SpecificInstrument | undefined, opts: CallbackOptions) => void): this;
  on(event: "rsp-qry-multicast-instrument", cb: (data: MulticastInstrument | undefined, opts: CallbackOptions) => void): this;
  on(event: "rsp-error", cb: (data: undefined, opts: CallbackOptions) => void): this;
  on(event: string, cb: (...args: never[]) => void): this;
  on(event: string, cb: (...args: never[]) => void): this {
    return super.on(event, cb as (...args: unknown[]) => void);
  }
}
