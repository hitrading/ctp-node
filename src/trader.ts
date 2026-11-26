/*
 * trader.ts - the Trader client (交易).
 *
 * Inherits every generated reqXxx() (typed input, Promise output) from
 * TraderBase, and adds pre-trade risk controls (enforced in C++ on the order
 * path) plus typed streaming events.
 */

import { native } from "./native-binding.js";
import { groupCtpInstrumentsByProduct, selectCtpProductRate } from "./product-rates.js";
import type { CtpResolvedMarginRate } from "./product-rates.js";
import { isTransientQueryFailure, queryIntervalMs, QUERY_RETRY_DELAY_MS } from "./query-pacing.js";
import { TraderBase, TRADER_EVENTS, TraderEvent } from "./generated/trader.gen.js";
import { STRUCT_ID } from "./generated/structs.gen.js";
import type { CallbackOptions } from "./client.js";
import type { FrontInfo, Order, Trade, InputOrder, RspUserLogin, Instrument } from "./generated/structs.gen.js";
import type { MarketData } from "./market-data.js";
import {
  copyDenseStringArray,
  copyOwnFronts,
  copyOwnRecord,
  ownStringKeys,
} from "./validation.js";

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

/** Pre-trade risk limits, enforced in C++ before each order is sent. */
export interface RiskLimits {
  /** Max volume per single order. Omit/0 = disabled. */
  maxOrderVolume?: number;
  /** Reject if order price deviates from the reference by > this ratio (e.g.
   *  0.02 = 2%). Feed the reference via trackMarketData(md) (the MD snapshot is
   *  read in C++); missing or stale data rejects opening orders. */
  maxPriceDeviation?: number;
  /** Max futures notional (price × volume × multiplier) per opening order.
   *  Unknown/unsupported product metadata fails closed. Omit/0 = disabled. */
  maxNotional?: number;
  /** Max opening-order sends per second (token bucket). Omit/0 = disabled. */
  maxOrdersPerSec?: number;
  /** Token-bucket burst; defaults to maxOrdersPerSec. */
  orderBurst?: number;
  /** Independent rate for risk-reducing close inserts. Disabled by default so
   *  emergency exits are not throttled by the opening-order bucket. */
  maxCloseOrdersPerSec?: number;
  /** Close-order token-bucket burst; defaults to maxCloseOrdersPerSec. */
  closeOrderBurst?: number;
  /** Maximum age of the market-data reference used by the deviation guard.
   *  Default 3000 ms. Missing or older data fails closed when the guard is on. */
  maxReferenceAgeMs?: number;
}

/** A bounded margin-rate sync for one hedge flag. The resolved schedules are
 * read back with {@link Trader.readMarginRates}. */
export interface MarginRateSyncOptions {
  instrumentIds: string[];
  hedgeFlag: "1" | "2" | "3" | "5" | "6" | "7";
}

/** The request-correlated identity established by a successful trader login. */
export interface TraderSessionIdentity {
  readonly frontId: number;
  readonly sessionId: number;
  /** MaxOrderRef reported by this login response. The process-wide automatic
   *  allocator may retain a higher high-water mark from earlier sends. */
  readonly maxOrderRef: number;
}

export interface TraderSessionResult {
  identity: TraderSessionIdentity;
  multipliers: number;
  marginRates: number;
}

/** Options for the one-call post-connect handshake, {@link Trader.session}. */
export interface SessionOptions {
  brokerId: string;
  userId: string;
  /** Trading account. Defaults to userId for ordinary retail accounts; set it
   *  explicitly when the login operator UserID differs from InvestorID. */
  investorId?: string;
  password: string;
  /** Terminal authentication (real accounts require it; SimNow uses
   *  appId "simnow_client_test" / authCode of 16 zeros). Omit both to skip. */
  appId?: string;
  authCode?: string;
  /** Confirm the settlement statement after login — real accounts must do this
   *  before trading (brokers reject orders otherwise). Default true; set false
   *  for environments that don't need it. */
  confirmSettlement?: boolean;
  /** Which metadata to fetch after login. Multipliers default on; margin rates
   *  require an explicit contract list and hedge flag.
   *  `multipliers` may be `true` (all instruments) or a list of symbols. */
  sync?: {
    multipliers?: boolean | string[];
    /** Contracts and hedge flag whose margin schedules should be queried.
     *  Omitted by default because CTP requires one rate query per contract. */
    marginRates?: MarginRateSyncOptions;
  };
}

/** Reject a non-finite or negative risk/cap limit. NaN/Inf can void ordinary
 *  comparisons, while a mistyped negative must not silently disable a control.
 *  Exactly zero is the explicit disabled/clear value. */
function assertFiniteLimit(name: string, v: number | undefined): void {
  if (v !== undefined && (!Number.isFinite(v) || v < 0)) {
    throw new TypeError(`${name} must be a non-negative finite number (got ${v})`);
  }
}

function assertPositiveInt32OrDisabled(name: string, v: number | undefined): void {
  assertFiniteLimit(name, v);
  if (v !== undefined && v > 0 && (!Number.isInteger(v) || v > 0x7fffffff)) {
    throw new TypeError(`${name} must be an integer in 1..2147483647 (got ${v})`);
  }
}

const RISK_LIMIT_KEYS = new Set<keyof RiskLimits>([
  "maxOrderVolume", "maxPriceDeviation", "maxNotional",
  "maxOrdersPerSec", "orderBurst", "maxCloseOrdersPerSec",
  "closeOrderBurst", "maxReferenceAgeMs",
]);

function assertObjectKeys(
  value: unknown,
  label: string,
  allowed: ReadonlySet<string>
): asserts value is Record<string, unknown> {
  for (const key of ownStringKeys(value, label))
    if (!allowed.has(key)) throw new TypeError(`${label}: unknown field ${key}`);
}

const SESSION_KEYS = new Set([
  "brokerId", "userId", "investorId", "password", "appId", "authCode",
  "confirmSettlement", "sync",
]);
const SESSION_SYNC_KEYS = new Set(["multipliers", "marginRates"]);
const MARGIN_RATE_SYNC_KEYS = new Set(["instrumentIds", "hedgeFlag"]);

export class Trader extends TraderBase {
  private readonly resolvedMarginRates = new Map<string, CtpResolvedMarginRate>();
  private resolvedMarginContext = 0;
  private broker?: string;
  private investor?: string;
  private orderRefSeq = 0;
  private queryRetryDelayMs = QUERY_RETRY_DELAY_MS;

  constructor(flowPath: string, fronts: string | string[]) {
    super(
      new native.Trader(flowPath, copyOwnFronts(fronts, "Trader fronts")),
      native.__layoutData(),
      TRADER_EVENTS
    );
    const clearConfirmedIdentity = () => {
      this.resolvedMarginRates.clear();
      this.broker = undefined;
      this.investor = undefined;
    };
    this.on("front-connected", clearConfirmedIdentity);
    this.on("front-disconnected", clearConfirmedIdentity);
    this.start();
  }

  /** A raw login Promise may resolve only with a structurally usable session.
   * Account confirmation itself happens in session(), after request-vs-response
   * identity validation; merely observing a protocol event never grants order
   * authority. */
  protected override onRequestResolved(
    eventName: string | undefined,
    value: unknown,
    _options: CallbackOptions
  ): void {
    if (eventName === "rsp-user-logout") {
      this.broker = undefined;
      this.investor = undefined;
      return;
    }
    if (eventName !== "rsp-user-login") return;
    // A matched login response updates the confirmed identity, but only from a
    // row carrying the COMPLETE, usable identity (FrontID > 0, non-zero signed
    // SessionID, plus
    // BrokerID/UserID) — the same acceptance condition the generated login
    // validator enforces before it resolves. A partial/abnormal success row
    // (e.g. FrontID 0) is rejected downstream, so caching broker/investor from
    // it would strand a stale identity after the login promise rejects, breaking
    // the invariant "broker/investor set ⟺ a confirmed login". We still never
    // throw here (that would reject an otherwise successful login); the
    // authoritative account-match + session-identity validation lives in
    // session() / _confirmSessionAccount, and direct callers stay fail-closed
    // natively via acceptsOrderAccount.
    const r = value as
      | {
          brokerId?: string;
          userId?: string;
          frontId?: unknown;
          sessionId?: unknown;
          maxOrderRef?: string;
        }
      | undefined;
    if (!r) return;
    if (
      typeof r.brokerId === "string" &&
      r.brokerId &&
      typeof r.userId === "string" &&
      r.userId &&
      Number.isInteger(r.frontId) &&
      Number(r.frontId) > 0 &&
      Number.isInteger(r.sessionId) &&
      Number(r.sessionId) !== 0
    ) {
      this.broker = r.brokerId;
      this.investor = r.userId;
      // The limits are valid only now that this session is logged in.
      this.queryIntervalMs = queryIntervalMs(this.frontInfo);
    }
    const maxOrderRef =
      typeof r.maxOrderRef === "string" ? r.maxOrderRef.trim() : "";
    if (/^\d{1,12}$/.test(maxOrderRef)) {
      const maxRef = Number(maxOrderRef);
      if (Number.isFinite(maxRef) && maxRef > this.orderRefSeq)
        this.orderRefSeq = maxRef;
    }
  }

  /** Generated login/logout methods call this only after their request has
   * encoded successfully. Revoke the confirmed JS account at that same atomic
   * transition boundary, so a concurrent sync helper cannot start with the old
   * identity while native risk state is already fail-closed for the new session. */
  protected override beginRequestContextTransition(reason: string): void {
    this.resolvedMarginRates.clear();
    this.broker = undefined;
    this.investor = undefined;
    super.beginRequestContextTransition(reason);
  }

  protected override shouldTimeoutRequest(
    requestId: number,
    sessionTransition: boolean
  ): boolean {
    if (!sessionTransition) return true;
    return this.native._timeoutSessionTransition(requestId) as boolean;
  }

  get apiVersion(): string {
    return this.native.apiVersion();
  }
  get tradingDay(): string {
    return this.native.tradingDay();
  }
  /** The connected front and the per-second limits it grants this session:
   *  `qryFreq` queries and `ftdPkgFreq` FTD packets (login, query, order and
   *  cancel alike). The limits are authoritative only after login. */
  get frontInfo(): FrontInfo {
    return this.native.frontInfo() as FrontInfo;
  }

  /**
   * One-call post-connect handshake. Call it from the `front-connected` handler
   * (and again after a reconnect): authenticate (if appId/authCode given) →
   * login → confirm settlement → sync multipliers / margin rates. Each
   * step just wraps the matching req / sync method, so you can hand-roll your
   * own flow instead when you need finer control. Returns the request-correlated
   * login identity and row counts from the sync steps. Must run on a connected
   * front (queries need the session up).
   */
  async session(opts: SessionOptions): Promise<TraderSessionResult> {
    assertObjectKeys(opts, "session options", SESSION_KEYS);
    const cleanOpts = copyOwnRecord(opts, "session options");
    const { brokerId, userId, password, appId, authCode } = cleanOpts;
    const investorId = cleanOpts.investorId ?? userId;
    if (typeof brokerId !== "string" || !brokerId ||
        brokerId.includes("\0") || typeof userId !== "string" || !userId ||
        userId.includes("\0") || typeof password !== "string" || !password ||
        password.includes("\0")) {
      throw new TypeError("session requires brokerId, userId and password");
    }
    if (typeof investorId !== "string" || !investorId ||
        investorId.includes("\0"))
      throw new TypeError("session investorId must be a non-empty string");
    if (cleanOpts.confirmSettlement !== undefined && typeof cleanOpts.confirmSettlement !== "boolean")
      throw new TypeError("session.confirmSettlement must be boolean");
    if ((appId === undefined) !== (authCode === undefined)) {
      throw new TypeError("session appId and authCode must be supplied together");
    }
    if (appId !== undefined && (typeof appId !== "string" || !appId ||
        appId.includes("\0") || typeof authCode !== "string" || !authCode ||
        authCode.includes("\0")))
      throw new TypeError("session appId and authCode must be non-empty strings");
    let cleanSync: NonNullable<SessionOptions["sync"]> = {};
    if (cleanOpts.sync !== undefined) {
      assertObjectKeys(cleanOpts.sync, "session.sync", SESSION_SYNC_KEYS);
      cleanSync = copyOwnRecord(cleanOpts.sync, "session.sync");
      const { multipliers, marginRates } = cleanSync;
      if (multipliers !== undefined && typeof multipliers !== "boolean")
        cleanSync.multipliers = copyDenseStringArray(
          multipliers, "session.sync.multipliers"
        );
      if (marginRates !== undefined) {
        assertObjectKeys(
          marginRates, "session.sync.marginRates", MARGIN_RATE_SYNC_KEYS
        );
        const cleanMarginRates = copyOwnRecord(
          marginRates, "session.sync.marginRates"
        );
        const instrumentIds = copyDenseStringArray(
          cleanMarginRates.instrumentIds,
          "session.sync.marginRates.instrumentIds",
          false
        );
        const hedgeFlag = cleanMarginRates.hedgeFlag;
        if (typeof hedgeFlag !== "string" || !/^[123567]$/.test(hedgeFlag)) {
          throw new TypeError(
            "session.sync.marginRates.hedgeFlag must be 1, 2, 3, 5, 6 or 7"
          );
        }
        cleanSync.marginRates = {
          instrumentIds,
          hedgeFlag: hedgeFlag as MarginRateSyncOptions["hedgeFlag"],
        };
      }
    }
    // Validate the exact CTP wire capacities before revoking a healthy prior
    // context. A malformed/oversized handshake must be side-effect free.
    if (appId !== undefined && authCode !== undefined)
      this.encode(STRUCT_ID.ReqAuthenticate,
        { brokerId, userId, appId, authCode });
    this.encode(STRUCT_ID.ReqUserLogin, { brokerId, userId, password });
    // Encode unconditionally: InvestorID is part of the native account
    // confirmation even when this environment skips settlement confirmation.
    // Passing these exact CTP bytes also avoids re-encoding GB18030 identities
    // through N-API's UTF-8 string conversion.
    const encodedAccount = this.encode(STRUCT_ID.SettlementInfoConfirm,
      { brokerId, investorId });
    // Revoke native session authority at the start of the whole handshake,
    // including the authentication phase. A failed auth must not leave an old
    // account able to send orders.
    this.native._beginSessionHandshake();
    // No query created under the previous login may cross this handshake. Do
    // this only after all synchronous option validation has succeeded.
    this.invalidateRequestContext("trader session started");
    let contextToken = this.requestContextToken();
    // The cached JS identity is confirmed-session state, not merely the last
    // attempted credentials. Revoke it before auth/login so a failed handshake
    // cannot drive a later snapshot query with stale account fields.
    this.broker = undefined;
    this.investor = undefined;
    if (appId !== undefined && authCode !== undefined) {
      await this.reqAuthenticate({ brokerId, userId, appId, authCode });
      this.assertRequestContext(contextToken, "trader session authentication");
    }
    // The generated login method deliberately begins exactly one new request
    // context. A test double may omit that transition; either form is safe. By
    // retaining the pre-call epoch unless the explicit-transition counter also
    // advanced once, an inline front-disconnected callback cannot masquerade as
    // the expected login transition.
    const beforeLoginContext = this.requestContextToken();
    const beforeLoginTransition = this.requestTransitionToken();
    const loginRequest = this.reqUserLogin({ brokerId, userId, password });
    const afterLoginContext = this.requestContextToken();
    const afterLoginTransition = this.requestTransitionToken();
    if (afterLoginTransition === beforeLoginTransition + 1 &&
        afterLoginContext === beforeLoginContext + 1) {
      contextToken = afterLoginContext;
    } else if (afterLoginTransition === beforeLoginTransition) {
      // Test doubles normally do not implement the generated transition. Keep
      // the old epoch so any synchronous front event is detected after await.
      contextToken = beforeLoginContext;
    } else {
      // Settle the request before surfacing the invariant violation so a
      // synchronously rejected test double cannot become an unhandled promise.
      try { await loginRequest; } catch { /* reported as transition failure */ }
      throw new Error("trader session login crossed multiple context transitions");
    }
    const login = await loginRequest as
      RspUserLogin | undefined;
    this.assertRequestContext(contextToken, "trader session login");
    if (!login || !Number.isInteger(login.frontId) || login.frontId <= 0 ||
        !Number.isInteger(login.sessionId) || login.sessionId === 0 ||
        login.brokerId !== brokerId ||
        login.userId !== userId) {
      // FrontID/SessionID identify this session's orders. Treating an empty
      // success response as ready would reuse an old/zero native session key.
      this.broker = undefined;
      this.investor = undefined;
      throw new Error("trader login response has missing or mismatched session identity");
    }
    const maxOrderRef = typeof login.maxOrderRef === "string"
      ? login.maxOrderRef.trim() : "";
    if (maxOrderRef !== "" && !/^\d{1,12}$/.test(maxOrderRef)) {
      this.broker = undefined;
      this.investor = undefined;
      throw new Error("trader login response has an invalid MaxOrderRef");
    }
    const maxRef = maxOrderRef === "" ? 0 : Number(maxOrderRef);
    // A successful CTP response may legally end with an empty payload. The
    // request identity is still authoritative for the synchronization calls
    // below; never retain a previous session's broker/investor in that case.
    const confirmed = this.native._confirmSessionAccount(
      login.frontId, login.sessionId, encodedAccount
    ) as boolean;
    if (!confirmed) {
      this.broker = undefined;
      this.investor = undefined;
      throw new Error("native Trader session/account confirmation failed");
    }
    this.broker = brokerId;
    this.investor = investorId;
    if (maxRef > this.orderRefSeq) this.orderRefSeq = maxRef;
    if (cleanOpts.confirmSettlement ?? true) {
      // idempotent: confirming an already-confirmed statement is fine
      await this.reqSettlementInfoConfirm({ brokerId, investorId });
      this.assertRequestContext(contextToken, "trader session settlement confirmation");
    }
    const sync = cleanSync;
    const mul = sync.multipliers ?? true;
    const result: TraderSessionResult = {
      identity: Object.freeze({
        frontId: login.frontId,
        sessionId: login.sessionId,
        maxOrderRef: maxRef,
      }),
      multipliers: 0,
      marginRates: 0,
    };
    if (mul) {
      result.multipliers = await this.syncMultipliers(Array.isArray(mul) ? mul : undefined);
      this.assertRequestContext(contextToken, "trader session multiplier sync");
    }
    const marginRateSync = sync.marginRates;
    if (marginRateSync !== undefined) {
      result.marginRates = await this.syncMarginRates(
        marginRateSync.instrumentIds,
        marginRateSync.hedgeFlag
      );
      this.assertRequestContext(contextToken, "trader session margin sync");
    }
    return result;
  }

  /** Assign a unique numeric OrderRef when the caller left it blank. CTP treats
   *  OrderRef as a per-session sequence and rejects a duplicate or non-numeric
   *  ref with "不允许重复报单", so an unset ref would otherwise collide. */
  private withAutoOrderRef(order: Partial<InputOrder>): Partial<InputOrder> {
    if (order.orderRef === undefined || order.orderRef === "") {
      if (this.orderRefSeq >= 999_999_999_999) {
        throw new RangeError("OrderRef space exhausted for this process/session");
      }
      return { ...order, orderRef: String(this.orderRefSeq + 1) };
    }
    if (typeof order.orderRef !== "string" || !/^\d{1,12}$/.test(order.orderRef)) {
      throw new TypeError("orderRef must contain 1..12 decimal digits");
    }
    const explicit = Number(order.orderRef);
    if (explicit <= this.orderRefSeq) {
      throw new RangeError(
        `explicit orderRef must be greater than the current high-water mark ${this.orderRefSeq}`
      );
    }
    return order;
  }

  private commitOrderRef(order: Partial<InputOrder>): void {
    this.orderRefSeq = Number(order.orderRef);
  }

  private validateOrder(order: Partial<InputOrder>, context: string): Partial<InputOrder> {
    const clean = copyOwnRecord(order, context);
    if (!this.broker || !this.investor) {
      throw new Error(`${context}: a confirmed Trader login is required`);
    }
    if (typeof clean.instrumentId !== "string" || !clean.instrumentId ||
        clean.instrumentId.includes("\0") ||
        (clean.direction !== "0" && clean.direction !== "1")) {
      throw new TypeError(`${context}: instrumentId and direction ("0"/"1") are required`);
    }
    if (typeof clean.combOffsetFlag !== "string" ||
        !/^[0-6]$/.test(clean.combOffsetFlag)) {
      throw new TypeError(`${context}: combOffsetFlag must be one CTP offset digit 0..6; multi-leg orders are unsupported`);
    }
    if (clean.combHedgeFlag !== undefined &&
        (typeof clean.combHedgeFlag !== "string" ||
         !/^[123567]$/.test(clean.combHedgeFlag))) {
      throw new TypeError(`${context}: combHedgeFlag must be one CTP hedge digit (1, 2, 3, 5, 6 or 7)`);
    }
    if (!Number.isInteger(clean.volumeTotalOriginal) ||
        !(Number(clean.volumeTotalOriginal) > 0) ||
        Number(clean.volumeTotalOriginal) > 0x7fffffff) {
      throw new TypeError(`${context}: volumeTotalOriginal must be a positive int32`);
    }
    if (!Number.isFinite(clean.limitPrice) || !(Number(clean.limitPrice) > 0) ||
        Number(clean.limitPrice) >= 1e300) {
      throw new TypeError(`${context}: limitPrice must be a positive finite price`);
    }
    if (clean.brokerId !== undefined && clean.brokerId !== this.broker) {
      throw new TypeError(`${context}: brokerId must match the confirmed login`);
    }
    if (clean.investorId !== undefined && clean.investorId !== this.investor) {
      throw new TypeError(`${context}: investorId must match the confirmed login`);
    }
    const value = <K extends keyof InputOrder>(key: K, fallback: InputOrder[K]) =>
      clean[key] === undefined ? fallback : clean[key];
    const empty = (v: unknown) => v === undefined || v === "";
    // The native gates judge one instrument at one limit price. Admit only
    // ordinary single-instrument limit orders with the GFD, FAK or FOK
    // execution policies (the same set decodeOrdinaryOrder accepts in C++).
    const timeCondition = value("timeCondition", "3");
    const volumeCondition = value("volumeCondition", "1");
    const supportedExecutionPolicy =
      (timeCondition === "3" && volumeCondition === "1") ||
      (timeCondition === "1" && (volumeCondition === "1" || volumeCondition === "3"));
    if (value("orderPriceType", "2") !== "2" ||
        !supportedExecutionPolicy || !empty(clean.gtdDate) ||
        value("minVolume", 1) !== 1 ||
        value("contingentCondition", "1") !== "1" ||
        value("stopPrice", 0) !== 0 ||
        value("forceCloseReason", "0") !== "0" ||
        value("isAutoSuspend", 0) !== 0 ||
        !empty(clean.businessUnit) || value("requestId", 0) !== 0 ||
        value("userForceClose", 0) !== 0 ||
        value("isSwapOrder", 0) !== 0 || !empty(clean.exchangeId) ||
        !empty(clean.investUnitId) || !empty(clean.accountId) ||
        !empty(clean.currencyId) || !empty(clean.clientId) ||
        value("sessionReqSeq", 0) !== 0) {
      throw new TypeError(
        `${context}: only single-instrument GFD, FAK or FOK limit orders are supported`
      );
    }
    return {
      ...clean,
      orderPriceType: "2",
      combHedgeFlag: clean.combHedgeFlag ?? "1",
      timeCondition,
      gtdDate: "",
      volumeCondition,
      minVolume: 1,
      contingentCondition: "1",
      stopPrice: 0,
      forceCloseReason: "0",
      isAutoSuspend: 0,
      businessUnit: "",
      requestId: 0,
      userForceClose: 0,
      isSwapOrder: 0,
      exchangeId: "",
      investUnitId: "",
      accountId: "",
      currencyId: "",
      clientId: "",
      sessionReqSeq: 0,
      brokerId: this.broker,
      investorId: this.investor,
    };
  }

  /**
   * Insert an order through the C++ pre-trade risk gate. Resolves on submission:
   * CTP sends no success acknowledgement for an accepted order - only
   * OnRtnOrder / OnRtnTrade (surfaced as the rtn-order / rtn-trade events;
   * correlate by orderRef). It rejects only if the send itself is refused (risk
   * gate, rate limit, or a CTP API error code). A blank orderRef is assigned a
   * unique numeric value automatically.
   */
  reqOrderInsert(req: Partial<InputOrder> = {}): Promise<void> {
    const order = this.withAutoOrderRef(this.validateOrder(req, "reqOrderInsert"));
    // Preflight field capacities before consuming the OrderRef high-water mark.
    // TraderBase encodes the same object again when it submits.
    this.encode(STRUCT_ID.InputOrder, order as Record<string, unknown>);
    const result = super.reqOrderInsert(order);
    this.commitOrderRef(order);
    return result;
  }

  /** Publish pre-trade risk limits to the C++ enforcer (takes effect at once). */
  setRiskLimits(config: RiskLimits): this {
    const clean = copyOwnRecord(config, "setRiskLimits");
    for (const key of ownStringKeys(clean, "setRiskLimits")) {
      if (!RISK_LIMIT_KEYS.has(key as keyof RiskLimits))
        throw new TypeError(`setRiskLimits: unknown field ${key}`);
    }
    // A non-finite limit would silently disable that control (see
    // assertFiniteLimit) — reject it rather than run unprotected.
    assertPositiveInt32OrDisabled("setRiskLimits.maxOrderVolume", clean.maxOrderVolume);
    assertFiniteLimit("setRiskLimits.maxPriceDeviation", clean.maxPriceDeviation);
    assertFiniteLimit("setRiskLimits.maxNotional", clean.maxNotional);
    assertFiniteLimit("setRiskLimits.maxOrdersPerSec", clean.maxOrdersPerSec);
    assertFiniteLimit("setRiskLimits.orderBurst", clean.orderBurst);
    assertFiniteLimit("setRiskLimits.maxCloseOrdersPerSec", clean.maxCloseOrdersPerSec);
    assertFiniteLimit("setRiskLimits.closeOrderBurst", clean.closeOrderBurst);
    assertFiniteLimit("setRiskLimits.maxReferenceAgeMs", clean.maxReferenceAgeMs);
    if (clean.maxReferenceAgeMs !== undefined && !(clean.maxReferenceAgeMs > 0)) {
      throw new TypeError("setRiskLimits.maxReferenceAgeMs must be > 0");
    }
    this.native.setRiskLimits(clean);
    return this;
  }
  /** Enter REDUCE_ONLY atomically at the final vendor-API send barrier:
   *  opening inserts are refused until resume(); closes still pass. */
  reduceOnly(): this {
    this.native.riskReduceOnly();
    return this;
  }
  /** Enter HALT_ALL atomically: both opening and closing order inserts, advanced
   *  mutations and bank/futures transfers are refused until resume().
   *  Cancellation actions remain available so working
   *  exchange orders can still be withdrawn. */
  haltAll(): this {
    this.native.riskHaltAll();
    return this;
  }
  /** Return to NORMAL mode after reduceOnly() or haltAll(). */
  resume(): this {
    this.native.riskResume();
    return this;
  }

  /** Use a MarketData feed as the live reference for the maxPriceDeviation and
   *  maxNotional checks. Wires the feed's C++ snapshot cache straight into this
   *  Trader's risk engine, so the checks read current prices in C++ on the order
   *  path (no JS round-trip). Without a fresh reference, an enabled check fails
   *  closed. */
  trackMarketData(md: MarketData): this {
    this.native._attachSnapshot(md._snapshotExternal());
    return this;
  }

  // Contract multipliers (for the maxNotional check) are NOT fed from JS: the
  // C++ risk engine sources them directly from CTP's OnRspQryInstrument
  // callback (see scripts/codegen/generate-trader.mjs), so any reqQryInstrument
  // keeps them current. A missing multiplier fails an enabled notional check
  // closed.

  /**
   * Run a CTP query for data that must exist (e.g. an instrument), asking again
   * when the counter is not ready: the local API refused the send, the counter
   * replied "not ready" / "busy", or the result is still empty. Retries pause
   * independently of the query spacing.
   */
  private async queryWithRetry<T>(
    query: () => Promise<T[]>,
    contextToken: number,
    attempts = 5
  ): Promise<T[]> {
    let lastErr: unknown;
    for (let i = 0; i < attempts; i++) {
      if (i > 0) await sleep(this.queryRetryDelayMs);
      try {
        this.assertRequestContext(contextToken, "query synchronization");
        const rows = await query();
        this.assertRequestContext(contextToken, "query synchronization");
        if (rows.length > 0) return rows;
      } catch (e) {
        this.assertRequestContext(contextToken, "query synchronization");
        // Do not turn a permanent validation/business/disconnect failure into
        // five delayed copies of the same request.
        if (!isTransientQueryFailure(e)) throw e;
        lastErr = e;
      }
    }
    if (lastErr) throw lastErr;
    throw new Error(`CTP query returned no rows after ${attempts} attempts`);
  }

  /** Query instruments so the C++ risk engine picks up their multipliers (it
   *  sources them directly from OnRspQryInstrument — no JS feed). No args queries
   *  all instruments (one request); otherwise the given symbols. Retries through
   *  cold-start flow control. Returns the count of contracts with a multiplier. */
  async syncMultipliers(instrumentIds?: string[]): Promise<number> {
    const contextToken = this.requestContextToken();
    if (instrumentIds !== undefined) {
      instrumentIds = copyDenseStringArray(
        instrumentIds, "syncMultipliers instrumentIds"
      );
      if (instrumentIds.length === 0)
        return 0; // explicit empty universe; only omitted means "query all"
    }
    let count = 0;
    const apply = (rows: unknown[]) => {
      const finalByInstrument = new Map<string, { instrumentId: string; volumeMultiple: number }>();
      for (const r of rows as Array<{ instrumentId: string; volumeMultiple: number }>)
        finalByInstrument.set(r.instrumentId, r);
      for (const r of finalByInstrument.values()) {
        if (r.instrumentId && Number.isFinite(r.volumeMultiple) &&
            r.volumeMultiple > 0 && r.volumeMultiple < 1e300) count++;
      }
    };
    if (instrumentIds) {
      for (const instrumentId of new Set(instrumentIds)) {
        apply(await this.queryWithRetry(async () =>
          ((await this.reqQryInstrument({ instrumentId })) as Array<{
            instrumentId: string; volumeMultiple: number;
          }>).filter((row) => row.instrumentId === instrumentId), contextToken));
      }
    } else {
      apply(await this.queryWithRetry(
        () => this.reqQryInstrument({}), contextToken
      ));
    }
    return count;
  }

  /** Query investor margin schedules for a bounded universe. The exchange-base
   *  query is issued only when the investor row is relative; the shared query
   *  scheduler enforces spacing. Returns the number of usable schedules. */
  async syncMarginRates(
    instrumentIds: string[],
    hedgeFlag: "1" | "2" | "3" | "5" | "6" | "7" = "1"
  ): Promise<number> {
    const contextToken = this.requestContextToken();
    instrumentIds = copyDenseStringArray(instrumentIds, "syncMarginRates", false);
    if (typeof hedgeFlag !== "string" || !/^[123567]$/.test(hedgeFlag)) {
      throw new TypeError("syncMarginRates hedgeFlag must be 1, 2, 3, 5, 6 or 7");
    }
    const brokerId = this.broker;
    const investorId = this.investor;
    if (!brokerId || !investorId) {
      throw new Error("syncMarginRates requires a successful trader login first");
    }
    for (const id of instrumentIds) this.resolvedMarginRates.delete(`${id}|${hedgeFlag}`);
    const completed: CtpResolvedMarginRate[] = [];
    // A fresh catalog supplies authoritative ProductID membership to JS and C++.
    // It is one request, independent of the number of traded months.
    const catalog = await this.queryWithRetry(() => this.reqQryInstrument({}), contextToken) as Instrument[];
    const productByInstrument = new Map(catalog.map(row => [row.instrumentId, row.productId]));
    const groups = groupCtpInstrumentsByProduct(instrumentIds, catalog);
    let count = 0;
    for (const [productId, members] of groups) {
      for (const [contractId, currentProduct] of productByInstrument)
        if (currentProduct === productId) this.resolvedMarginRates.delete(`${contractId}|${hedgeFlag}`);
      type InvestorMarginRow = {
        instrumentId: string;
        hedgeFlag: string;
        isRelative: number;
        longMarginRatioByMoney: number;
        longMarginRatioByVolume: number;
        shortMarginRatioByMoney: number;
        shortMarginRatioByVolume: number;
      };
      const investorRows = await this.queryWithRetry(async () =>
        ((await this.reqQryInstrumentMarginRate({
          brokerId,
          investorId,
          instrumentId: productId,
          hedgeFlag,
        })) as InvestorMarginRow[]).filter(
          (row) => row.hedgeFlag === hedgeFlag && (row.instrumentId === productId ||
            productByInstrument.get(row.instrumentId) === productId)
        ), contextToken);
      // The native map is keyed by instrument+hedge and processes callbacks in
      // order, so its final state is the final row, not "any usable row".
      const rates = new Map(investorRows.map(row => [row.instrumentId, row]));
      for (const instrumentId of members) {
      const investor = selectCtpProductRate(rates, instrumentId, productId);
      const relative = investor.isRelative === 1;
      type ExchangeMarginRow = {
        instrumentId: string;
        hedgeFlag: string;
        longMarginRatioByMoney: number;
        longMarginRatioByVolume: number;
        shortMarginRatioByMoney: number;
        shortMarginRatioByVolume: number;
      };
      const baseRows = relative
        ? await this.queryWithRetry(async () =>
            ((await this.reqQryExchangeMarginRate({
              brokerId,
              instrumentId,
              hedgeFlag,
            })) as ExchangeMarginRow[]).filter(
              (row) => row.instrumentId === instrumentId && row.hedgeFlag === hedgeFlag
            ), contextToken)
        : [];
      const base = baseRows.at(-1);
      const component = (value: number) =>
        Number.isFinite(value) && value >= 0 && value < 1e300;
      const scheduleUsable = (row: {
        longMarginRatioByMoney: number;
        longMarginRatioByVolume: number;
        shortMarginRatioByMoney: number;
        shortMarginRatioByVolume: number;
      } | undefined) => !!row &&
        component(row.longMarginRatioByMoney) &&
        component(row.longMarginRatioByVolume) &&
        component(row.shortMarginRatioByMoney) &&
        component(row.shortMarginRatioByVolume) &&
        (row.longMarginRatioByMoney > 0 || row.longMarginRatioByVolume > 0) &&
        (row.shortMarginRatioByMoney > 0 || row.shortMarginRatioByVolume > 0);
      const investorComponentsValid =
        component(investor.longMarginRatioByMoney) &&
        component(investor.longMarginRatioByVolume) &&
        component(investor.shortMarginRatioByMoney) &&
        component(investor.shortMarginRatioByVolume);
      if ((investor.isRelative === 0 || investor.isRelative === 1) && investorComponentsValid &&
          (relative ? scheduleUsable(base) : scheduleUsable(investor))) {
        const value: CtpResolvedMarginRate = { instrumentId, hedgeFlag,
          longMarginRatioByMoney: investor.longMarginRatioByMoney + (base?.longMarginRatioByMoney ?? 0),
          longMarginRatioByVolume: investor.longMarginRatioByVolume + (base?.longMarginRatioByVolume ?? 0),
          shortMarginRatioByMoney: investor.shortMarginRatioByMoney + (base?.shortMarginRatioByMoney ?? 0),
          shortMarginRatioByVolume: investor.shortMarginRatioByVolume + (base?.shortMarginRatioByVolume ?? 0) };
        if (scheduleUsable(value)) { completed.push(value); count++; }
      }
      }
    }
    if (count === new Set(instrumentIds).size) {
      this.resolvedMarginContext = contextToken;
      for (const rate of completed) this.resolvedMarginRates.set(`${rate.instrumentId}|${hedgeFlag}`, rate);
    }
    return count;
  }

  /** Only completed synchronization results from the current authenticated
   * session; failed/incomplete refreshes never expose previous schedules. */
  readMarginRates(instrumentIds: readonly string[], hedgeFlag: CtpResolvedMarginRate['hedgeFlag']): CtpResolvedMarginRate[] {
    this.assertRequestContext(this.resolvedMarginContext, 'margin snapshot');
    return instrumentIds.map(instrumentId => {
      const rate = this.resolvedMarginRates.get(`${instrumentId}|${hedgeFlag}`);
      if (!rate) throw new Error(`CTP synchronized margin rate is missing: ${instrumentId}/${hedgeFlag}`);
      return { ...rate };
    });
  }

  // Typed streaming events (the common ones; any event name still works).
  on(event: "rtn-order", cb: (data: Order, opts: CallbackOptions) => void): this;
  on(event: "rtn-trade", cb: (data: Trade, opts: CallbackOptions) => void): this;
  on(event: "front-connected", cb: (data: undefined, opts: CallbackOptions) => void): this;
  on(event: "front-disconnected", cb: (reason: number, opts: CallbackOptions) => void): this;
  on(event: "rsp-user-login", cb: (data: RspUserLogin | undefined, opts: CallbackOptions) => void): this;
  on(event: "err-rtn-order-insert", cb: (data: InputOrder | undefined, opts: CallbackOptions) => void): this;
  on(event: string, cb: (...args: never[]) => void): this;
  on(event: string, cb: (...args: never[]) => void): this {
    return super.on(event, cb as (...args: unknown[]) => void);
  }
}

export { TraderEvent };
