/*
 * trader.ts - the Trader client (交易).
 *
 * Inherits every generated reqXxx() (typed input, Promise output) from
 * TraderBase, and adds pre-trade risk controls (enforced in C++ on the order
 * path) plus typed streaming events.
 */

import { native } from "./native-binding.js";
import { TraderBase, TRADER_EVENTS, TraderEvent } from "./generated/trader.gen.js";
import { STRUCT_ID } from "./generated/structs.gen.js";
import type { CallbackOptions } from "./client.js";
import type { Order, Trade, InputOrder, RspUserLogin } from "./generated/structs.gen.js";
import type { MarketData } from "./market-data.js";
import {
  assertPlainRecord,
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
  /** Cap on total open-position MARGIN (Σ open price × volume × multiplier ×
   *  absolute by-money/by-volume margin schedule) — a zero-lag hard cap enforced in
   *  C++ on every opening order (regular AND armed). 0/undefined = disabled.
   *  Margin rates are fed automatically from CTP's OnRspQryInstrumentMarginRate
   *  inside C++ (populate them by calling reqQryInstrumentMarginRate); until a
   *  multiplier or usable absolute schedule is missing, opening orders fail
   *  closed. Both by-money and per-lot components are included; relative
   *  investor schedules are combined with their exchange base schedule.
   *  Multipliers / existing positions are picked up via syncMultipliers() /
   *  syncPositions(). */
  maxAccountMargin?: number;
}

/** A per-instrument lot cap: a single number caps both sides at that value;
 *  `{ long, short }` caps each side separately (omit a side to leave it open). */
export type InstrumentPositionLimit =
  number | { long?: number; short?: number };

/** One bucket in a complete manually reconciled held-position snapshot. */
export interface PositionSeed {
  instrumentId: string;
  side: "long" | "short";
  hedgeFlag: "1" | "2" | "3" | "5" | "6" | "7";
  volume: number;
  margin: number;
}

/** A bounded margin-rate sync scoped to the hedge flag used by the orders that
 * will consume those schedules in the native risk ledger. */
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
  positions: number;
  orders: number;
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
  /** Which risk inputs to fetch after login. Multipliers/positions/orders default
   *  on; margin rates require an explicit contract list and hedge flag.
   *  `multipliers` may be `true` (all instruments) or a list of symbols. */
  sync?: {
    multipliers?: boolean | string[];
    /** Contracts and hedge flag whose margin schedules should be queried.
     *  Omitted by default because CTP requires one rate query per contract. */
    marginRates?: MarginRateSyncOptions;
    positions?: boolean;
    orders?: boolean;
  };
}

/** A latency-critical armed order: evaluated and fired in C++ on the market-data
 *  callback thread the instant its trigger hits — JS is never in the hot path. */
export interface ArmSpec {
  instrumentId: string;
  /** Buy fires when ask ≤ triggerPrice; sell when bid ≥ triggerPrice. */
  side: "buy" | "sell";
  triggerPrice: number;
  /** The order to send when triggered (a full InputOrder; set an OrderRef to
   *  correlate the resulting rtn-order). */
  order: Partial<InputOrder>;
}

export interface ArmHandle {
  readonly id: number;
  /** Remove the trigger. Returns false if it was already gone. */
  disarm(): boolean;
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

function assertLotLimit(name: string, v: number | undefined): void {
  assertFiniteLimit(name, v);
  if (v !== undefined && v > 0 &&
      (!Number.isInteger(v) || v > 0x7fffffff)) {
    throw new TypeError(`${name} must be an integer in 1..2147483647 lots`);
  }
}

function assertInstrumentId(
  name: string,
  instrumentId: unknown
): asserts instrumentId is string {
  if (typeof instrumentId !== "string" || !instrumentId || instrumentId.includes("\0"))
    throw new TypeError(`${name}: instrumentId must be non-empty and contain no NUL`);
}

const RISK_LIMIT_KEYS = new Set<keyof RiskLimits>([
  "maxOrderVolume", "maxPriceDeviation", "maxNotional",
  "maxOrdersPerSec", "orderBurst", "maxCloseOrdersPerSec",
  "closeOrderBurst", "maxReferenceAgeMs", "maxAccountMargin",
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
const SESSION_SYNC_KEYS = new Set(["multipliers", "marginRates", "positions", "orders"]);
const MARGIN_RATE_SYNC_KEYS = new Set(["instrumentIds", "hedgeFlag"]);
const ARM_SPEC_KEYS = new Set(["instrumentId", "side", "triggerPrice", "order"]);
const POSITION_LIMIT_KEYS = new Set(["long", "short"]);
const POSITION_SEED_KEYS = new Set([
  "instrumentId", "side", "hedgeFlag", "volume", "margin",
]);

export class Trader extends TraderBase {
  private broker?: string;
  private investor?: string;
  private orderRefSeq = 0;

  constructor(flowPath: string, fronts: string | string[]) {
    super(
      new native.Trader(flowPath, copyOwnFronts(fronts, "Trader fronts")),
      native.__layoutData(),
      TRADER_EVENTS
    );
    const clearConfirmedIdentity = () => {
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
    // authoritative account-match + reservation-key validation lives in
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

  /**
   * One-call post-connect handshake. Call it from the `front-connected` handler
   * (and again after a reconnect): authenticate (if appId/authCode given) →
   * login → confirm settlement → sync multipliers / margin rates / orders /
   * positions. Each
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
      const { multipliers, marginRates, positions, orders } = cleanSync;
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
      if (positions !== undefined && typeof positions !== "boolean")
        throw new TypeError("session.sync.positions must be boolean");
      if (orders !== undefined && typeof orders !== "boolean")
        throw new TypeError("session.sync.orders must be boolean");
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
    // Revoke native mandates/session authority at the start of the whole
    // handshake, including the authentication phase. A failed auth must not
    // leave an old close arm capable of firing under a superseded account.
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
    // A reconnect may have an event gap. Keep all opening inserts
    // fail-closed throughout the handshake until both authoritative snapshots
    // have independently committed.
    this.native._markPositionLedgerUncertain();
    this.native._markOrderLedgerUncertain();
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
      // FrontID/SessionID are part of the reservation identity. Treating an
      // empty success response as ready would reuse an old/zero native session
      // key and create ghost reservations when the first OnRtnOrder arrives.
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
      positions: 0,
      orders: 0,
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
    if (sync.orders ?? true) {
      result.orders = await this.syncOrders();
      this.assertRequestContext(contextToken, "trader session order sync");
    }
    if (sync.positions ?? true) {
      result.positions = await this.syncPositions();
      this.assertRequestContext(contextToken, "trader session position sync");
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
    // The reservation ledger models ordinary single-instrument limit orders.
    // Admit only the three execution policies whose live remainder is fully
    // represented by CTP order/trade callbacks: GFD, FAK and FOK.
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
    assertFiniteLimit("setRiskLimits.maxAccountMargin", clean.maxAccountMargin);
    assertFiniteLimit("setRiskLimits.maxReferenceAgeMs", clean.maxReferenceAgeMs);
    if (clean.maxReferenceAgeMs !== undefined && !(clean.maxReferenceAgeMs > 0)) {
      throw new TypeError("setRiskLimits.maxReferenceAgeMs must be > 0");
    }
    this.native.setRiskLimits(clean);
    return this;
  }
  /** Enter REDUCE_ONLY atomically at the final vendor-API send barrier. Armed
   *  opens remain registered but cannot send until resume(). */
  reduceOnly(): this {
    this.native.riskReduceOnly();
    return this;
  }
  /** Enter HALT_ALL atomically: both opening and closing order inserts, advanced
   *  mutations and bank/futures transfers are refused, and all existing armed
   *  triggers are cleared. New arm() calls are rejected until resume().
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

  /** Use a MarketData feed as the live reference for the maxPriceDeviation check.
   *  Wires the feed's C++ snapshot cache straight into this Trader's risk engine,
   *  so the deviation check reads current prices in C++ on the order path (no JS
   *  round-trip, and it also covers armed orders that fire with no JS in the loop).
   *  Without a fresh reference, an enabled deviation check fails closed. */
  trackMarketData(md: MarketData): this {
    this.native._attachSnapshot(md._snapshotExternal());
    return this;
  }

  // Contract multipliers (for cost accounting) and margin rates (for the
  // maxAccountMargin cap) are NOT fed from JS: the C++ risk engine sources them directly
  // from CTP's OnRspQryInstrument / OnRspQryInstrumentMarginRate callbacks (see
  // scripts/codegen/generate-trader.mjs). So any reqQryInstrument keeps
  // multipliers current, and querying reqQryInstrumentMarginRate for the
  // syncMarginRates() loads investor plus exchange-base schedules. Missing
  // metadata fails closed; IsRelative is combined with the matching base.

  /**
   * Cap the open position (in lots) for one instrument, enforced in C++ on
   * every opening order. Pass a number to cap both sides at once, or
   * `{ long, short }` to cap each side separately. Within `{ long, short }`,
   * **omitting a side leaves that side's current cap unchanged** (on a re-call it
   * is NOT reset to uncapped), and passing `0` for a side removes its cap.
   * Long and short are always tracked independently.
   *
   * Held positions and in-flight working opens are counted atomically.
   *
   *   td.setInstrumentPositionLimit("rb2610", 100);
   *   td.setInstrumentPositionLimit("ru2610", { long: 100, short: 20 });
   */
  setInstrumentPositionLimit(
    instrumentId: string,
    limit: InstrumentPositionLimit
  ): this {
    assertInstrumentId("setInstrumentPositionLimit", instrumentId);
    const row = this.normalizePositionLimit(instrumentId, limit);
    this.native._setInstrumentPositionLimits([row]);
    return this;
  }

  /** Cap several instruments at once, e.g.
   *  `{ rb2610: 100, ru2610: { long: 100, short: 20 } }`. */
  setInstrumentPositionLimits(
    limits: Record<string, InstrumentPositionLimit>
  ): this {
    const rows = ownStringKeys(limits, "setInstrumentPositionLimits").map((instrumentId) => {
      const limit = limits[instrumentId];
      assertInstrumentId("setInstrumentPositionLimits", instrumentId);
      return this.normalizePositionLimit(instrumentId, limit);
    });
    this.native._setInstrumentPositionLimits(rows);
    return this;
  }

  private normalizePositionLimit(
    instrumentId: string,
    limit: InstrumentPositionLimit
  ): { instrumentId: string; long?: number; short?: number } {
    if (typeof limit === "number") {
      assertLotLimit("setInstrumentPositionLimit", limit);
      return copyOwnRecord(
        { instrumentId, long: limit, short: limit },
        "setInstrumentPositionLimit"
      );
    }
    assertPlainRecord(limit, "setInstrumentPositionLimit");
    const clean = copyOwnRecord(limit, "setInstrumentPositionLimit");
    if (clean.long === undefined && clean.short === undefined) {
      throw new TypeError("setInstrumentPositionLimit requires a number or at least one of long/short");
    }
    for (const key of ownStringKeys(clean, "setInstrumentPositionLimit")) {
      if (!POSITION_LIMIT_KEYS.has(key))
        throw new TypeError(`setInstrumentPositionLimit: unknown field ${key}`);
    }
    assertLotLimit("setInstrumentPositionLimit.long", clean.long);
    assertLotLimit("setInstrumentPositionLimit.short", clean.short);
    return copyOwnRecord(
      { instrumentId, long: clean.long, short: clean.short },
      "setInstrumentPositionLimit"
    );
  }

  /**
   * Cap one instrument's scheduled open margin (CTP by-money + by-volume,
   * long and short summed), enforced in C++ on every opening order. This is
   * independent of the account-wide maxAccountMargin cap; both apply. Pass
   * `limit = 0` to remove it. Held and in-flight exposure both count.
   */
  setInstrumentMarginLimit(instrumentId: string, limit: number): this {
    assertInstrumentId("setInstrumentMarginLimit", instrumentId);
    assertFiniteLimit("setInstrumentMarginLimit", limit);
    this.native._setInstrumentMarginLimits([{ instrumentId, limit }]);
    return this;
  }

  /** Cap several instruments' scheduled open margin at once, e.g.
   *  `{ ag2608: 2_000_000, au2608: 5_000_000 }`. */
  setInstrumentMarginLimits(limits: Record<string, number>): this {
    const rows = ownStringKeys(limits, "setInstrumentMarginLimits").map((instrumentId) => {
      const limit = limits[instrumentId];
      assertInstrumentId("setInstrumentMarginLimits", instrumentId);
      assertFiniteLimit(`setInstrumentMarginLimits.${instrumentId}`, limit);
      return { instrumentId, limit };
    });
    this.native._setInstrumentMarginLimits(rows);
    return this;
  }

  /** Atomically replace held positions from a complete manually reconciled
   * snapshot. `margin` is CTP InvestorPosition.UseMargin. An empty array is an
   * authoritative flat book. Partial/incremental seeding is intentionally not
   * exposed because armed orders can run between JS calls. */
  seedPositions(positions: ReadonlyArray<PositionSeed>): this {
    if (!Array.isArray(positions))
      throw new TypeError("seedPositions requires a complete position array");
    const seeds: Array<{
      instrumentId: string; isLong: boolean; hedgeFlag: string;
      volume: number; margin: number;
    }> = [];
    const positionCount = positions.length;
    for (let i = 0; i < positionCount; i++) {
      if (!Object.prototype.hasOwnProperty.call(positions, i))
        throw new TypeError(`seedPositions row ${i} must be an explicit array entry`);
      const row = copyOwnRecord(positions[i], `seedPositions row ${i}`);
      for (const key of ownStringKeys(row, `seedPositions row ${i}`))
        if (!POSITION_SEED_KEYS.has(key))
          throw new TypeError(`seedPositions row ${i}: unknown field ${key}`);
      assertInstrumentId("seedPositions", row.instrumentId);
      if ((row.side !== "long" && row.side !== "short") ||
          typeof row.hedgeFlag !== "string" ||
          !/^[123567]$/.test(row.hedgeFlag) ||
          !Number.isInteger(row.volume) || row.volume < 0 ||
          !Number.isFinite(row.margin) || row.margin < 0) {
        throw new TypeError(
          "seedPositions requires long/short, one hedgeFlag digit, a non-negative integer volume and non-negative finite margin"
        );
      }
      seeds.push({
        instrumentId: row.instrumentId,
        isLong: row.side === "long",
        hedgeFlag: row.hedgeFlag,
        volume: row.volume,
        margin: row.margin,
      });
    }
    return this.replacePositionSeeds(seeds);
  }

  private replacePositionSeeds(
    seeds: Array<{
      instrumentId: string; isLong: boolean; hedgeFlag: string;
      volume: number; margin: number;
    }>
  ): this {
    for (let attempt = 0; attempt < 5; attempt++) {
      const revision: number = this.native.riskRevision();
      if (this.native.replacePositions(seeds, revision) as boolean)
        return this;
    }
    throw new Error("position snapshot could not stabilize; live state kept unchanged");
  }

  /** Atomically replace held position margin from complete gross-mode query rows
   *  (`posiDirection` '2' long, '3' short). Net/unknown direction is rejected
   *  because guessing a side corrupts per-side volume caps. */
  seedFromPositions(
    positions: ReadonlyArray<{
      instrumentId: string;
      posiDirection: string;
      hedgeFlag: string;
      position: number;
      useMargin: number;
    }>
  ): this {
    const seeds = this.aggregatePositions(positions);
    // Native callbacks can mutate the ledger concurrently even though this JS
    // method is synchronous. Retry a revision-checked replacement rather than
    // expose a clear-then-seed gap or overwrite a callback that raced us.
    return this.replacePositionSeeds(seeds);
  }

  private aggregatePositions(
    positions: ReadonlyArray<{
      instrumentId: string;
      posiDirection: string;
      hedgeFlag: string;
      position: number;
      useMargin: number;
    }>
  ): Array<{ instrumentId: string; isLong: boolean; hedgeFlag: string; volume: number; margin: number }> {
    // Aggregate by instrument+side+hedge flag (CTP may return several rows per
    // bucket, e.g. today/history). Different hedge flags can have different
    // margin schedules and must never be averaged together on close.
    if (!Array.isArray(positions))
      throw new TypeError("position snapshot must be an array");
    const agg = new Map<string, { instrumentId: string; isLong: boolean; hedgeFlag: string; volume: number; margin: number }>();
    const positionCount = positions.length;
    for (let i = 0; i < positionCount; i++) {
      if (!Object.prototype.hasOwnProperty.call(positions, i))
        throw new TypeError(`position snapshot row ${i} must be an explicit array entry`);
      const p = positions[i];
      const plain = p !== null && typeof p === "object" && !Array.isArray(p) &&
        (Object.getPrototypeOf(p) === Object.prototype || Object.getPrototypeOf(p) === null);
      const row = plain ? copyOwnRecord(p, "position snapshot row") : undefined;
      if (!row || typeof row.instrumentId !== "string" || !row.instrumentId ||
          row.instrumentId.includes("\0") ||
          !Number.isInteger(row.position) || row.position < 0 ||
          !Number.isFinite(row.useMargin) || row.useMargin < 0 ||
          (row.posiDirection !== "2" && row.posiDirection !== "3") ||
          typeof row.hedgeFlag !== "string" ||
          !/^[123567]$/.test(row.hedgeFlag)) {
        throw new TypeError(`invalid position snapshot for ${row?.instrumentId || "<unknown>"}`);
      }
      if (!(row.position > 0)) continue;
      const isLong = row.posiDirection === "2";
      const key = `${row.instrumentId}|${isLong}|${row.hedgeFlag}`;
      const e = agg.get(key) ?? { instrumentId: row.instrumentId, isLong, hedgeFlag: row.hedgeFlag, volume: 0, margin: 0 };
      const volume = e.volume + row.position;
      const margin = e.margin + row.useMargin;
      if (!Number.isSafeInteger(volume) || !Number.isFinite(margin)) {
        throw new RangeError(`position snapshot overflow for ${row.instrumentId}`);
      }
      e.volume = volume;
      e.margin = margin; // real margin occupied (the unit the cap tracks)
      agg.set(key, e);
    }
    return [...agg.values()];
  }

  /** Current total open-position margin tracked by the C++ risk engine. */
  get accountMargin(): number {
    return this.native.accountMargin();
  }

  /** Clear held position state only and mark it uncertain. Working-order
   *  reservations are preserved; openings remain blocked until a complete
   *  seedFromPositions() or syncPositions() succeeds. */
  resetPositions(): this {
    this.native.resetPositions();
    return this;
  }

  /**
   * Run a CTP query with flow-control retries. CTP rate-limits queries (~1/sec)
   * and one issued too soon after login can be rejected or come back empty
   * before the account is ready. With `requireNonEmpty`, an empty result is
   * treated as not-ready-yet and retried (for data that must exist, e.g. an
   * instrument); otherwise empty is returned as-is (e.g. a genuinely flat
   * position book, so a flat account doesn't waste the full retry budget).
   */
  private async queryWithRetry<T>(
    query: () => Promise<T[]>,
    requireNonEmpty: boolean,
    contextToken: number,
    attempts = 5
  ): Promise<T[]> {
    let lastErr: unknown;
    for (let i = 0; i < attempts; i++) {
      try {
        this.assertRequestContext(contextToken, "query synchronization");
        // Every generated ReqQry* / ReqQuery* goes through queryRequest(), whose
        // shared scheduler enforces the interval. Do not sleep here as well or
        // retries are needlessly spaced twice.
        const rows = await query();
        this.assertRequestContext(contextToken, "query synchronization");
        if (!requireNonEmpty || rows.length > 0) return rows;
      } catch (e) {
        this.assertRequestContext(contextToken, "query synchronization");
        // Native send codes -2/-3 are the CTP transient flow-control cases.
        // Do not turn a permanent validation/business/disconnect failure into
        // five delayed copies of the same request.
        const code = (e as { ctpReturnCode?: unknown })?.ctpReturnCode;
        if (code !== -2 && code !== -3) throw e;
        lastErr = e;
      }
    }
    if (lastErr) throw lastErr;
    if (requireNonEmpty)
      throw new Error(`CTP query returned no rows after ${attempts} attempts`);
    /* c8 ignore next -- fixed positive attempts make this an internal invariant */
    throw new Error("query retry invariant violated");
  }

  /** Empty position/order snapshots are ambiguous immediately after login. Require
   *  the same empty result twice, one query interval apart, before treating it as
   *  authoritative. */
  private async authoritativeQuery<T>(
    query: () => Promise<T[]>,
    contextToken: number
  ): Promise<T[]> {
    const first = await this.queryWithRetry(query, false, contextToken);
    if (first.length > 0) return first;
    // The scheduler spaces this confirmation request from the first one.
    return this.queryWithRetry(query, false, contextToken);
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
          }>).filter((row) => row.instrumentId === instrumentId), true, contextToken));
      }
    } else {
      apply(await this.queryWithRetry(
        () => this.reqQryInstrument({}), true, contextToken
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
    let count = 0;
    for (const instrumentId of new Set(instrumentIds)) {
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
          instrumentId,
          hedgeFlag,
        })) as InvestorMarginRow[]).filter(
          (row) => row.instrumentId === instrumentId && row.hedgeFlag === hedgeFlag
        ), true, contextToken);
      // The native map is keyed by instrument+hedge and processes callbacks in
      // order, so its final state is the final row, not "any usable row".
      const investor = investorRows.at(-1)!;
      const relative = investor.isRelative !== 0;
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
            ), true, contextToken)
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
      if (investorComponentsValid &&
          (relative ? scheduleUsable(base) : scheduleUsable(investor)))
        count++;
    }
    return count;
  }

  /** Replace held position margin from CTP (`reqQryInvestorPosition`). The
   *  replacement is revision-checked: if an order/trade callback races the
   *  snapshot it retries without clearing live state or reservations. */
  async syncPositions(): Promise<number> {
    const contextToken = this.requestContextToken();
    if (arguments.length !== 0)
      throw new TypeError("syncPositions does not accept account overrides");
    const brokerId = this.broker;
    const investorId = this.investor;
    if (typeof brokerId !== "string" || !brokerId || brokerId.includes("\0") ||
        typeof investorId !== "string" || !investorId || investorId.includes("\0"))
      throw new Error("syncPositions requires a successful trader login");
    this.native._markPositionLedgerUncertain();
    type PositionRow = {
      brokerId: string;
      investorId: string;
      instrumentId: string;
      posiDirection: string;
      position: number;
      useMargin: number;
      hedgeFlag: string;
    };
    for (let attempt = 0; attempt < 5; attempt++) {
      this.assertRequestContext(contextToken, "position synchronization");
      const revision: number = this.native.riskRevision();
      const rows = await this.authoritativeQuery(async () => {
        const result = (await this.reqQryInvestorPosition({ brokerId, investorId })) as
          PositionRow[];
        if (result.some((row) => row.brokerId !== brokerId ||
                               row.investorId !== investorId)) {
          throw new Error("position snapshot contains a mismatched account identity");
        }
        return result;
      }, contextToken);
      const seeds = this.aggregatePositions(rows);
      this.assertRequestContext(contextToken, "position synchronization");
      if (this.native.replacePositions(seeds, revision) as boolean)
        return rows.filter((r) => r.position > 0).length;
      if (attempt < 4)
        await sleep(100); // live order/trade changed during the snapshot; retry
    }
    throw new Error("position snapshot could not stabilize; live state kept unchanged");
  }

  /**
   * Rebuild the in-flight reservation from CTP's currently-working orders
   * (reqQryOrder) - an authoritative resync. Call it after login and after any
   * reconnect so the position caps account for orders already working at the
   * broker: ones placed before a reconnect, or - since CTP delivers them too -
   * orders placed from another terminal on the same account. Returns the number
   * of working open orders re-reserved. Run syncMultipliers() first so the
   * reserved cost uses the right contract multiplier.
   */
  async syncOrders(): Promise<number> {
    const contextToken = this.requestContextToken();
    if (arguments.length !== 0)
      throw new TypeError("syncOrders does not accept account overrides");
    const brokerId = this.broker;
    const investorId = this.investor;
    if (typeof brokerId !== "string" || !brokerId || brokerId.includes("\0") ||
        typeof investorId !== "string" || !investorId || investorId.includes("\0"))
      throw new Error("syncOrders requires a successful trader login");
    // Order snapshots must precede the final held-position snapshot. Mark both
    // sides uncertain so calling syncPositions()->syncOrders() in the unsafe
    // order cannot reopen trading; a later syncPositions() is required.
    this.native._markPositionLedgerUncertain();
    this.native._markOrderLedgerUncertain();
    type OrderRow = {
      brokerId: string;
      investorId: string;
      frontId: number;
      sessionId: number;
      orderRef: string;
      tradingDay: string;
      exchangeId: string;
      orderSysId: string;
      instrumentId: string;
      combOffsetFlag: string;
      direction: string;
      combHedgeFlag: string;
      orderStatus: string;
      limitPrice: number;
      volumeTotalOriginal: number;
      volumeTraded: number;
    };
    for (let attempt = 0; attempt < 5; attempt++) {
      this.assertRequestContext(contextToken, "order synchronization");
      const revision: number = this.native.riskRevision();
      const rows = await this.authoritativeQuery(async () => {
        const result = (await this.reqQryOrder({ brokerId, investorId })) as
          OrderRow[];
        if (result.some((row) => row.brokerId !== brokerId ||
                               row.investorId !== investorId)) {
          throw new Error("order snapshot contains a mismatched account identity");
        }
        return result;
      }, contextToken);
      // Every status except the four definitive terminals is still live. This
      // includes conditional Unknown/NotTouched/Touched ('a'/'b'/'c').
      const terminal = new Set(["0", "2", "4", "5"]);
      const active = rows.filter((o) => !terminal.has(o.orderStatus));
      for (const o of active) {
        const vol = o.volumeTotalOriginal - o.volumeTraded;
        if (!Number.isInteger(o.frontId) || !Number.isInteger(o.sessionId) ||
            !o.orderRef || !o.instrumentId ||
            (o.direction !== "0" && o.direction !== "1") ||
             typeof o.combOffsetFlag !== "string" ||
             typeof o.combHedgeFlag !== "string" ||
            !Number.isInteger(o.volumeTotalOriginal) ||
            !Number.isInteger(o.volumeTraded) || o.volumeTraded < 0 ||
            o.volumeTraded > o.volumeTotalOriginal ||
            !Number.isInteger(vol) || vol < 0 ||
            !Number.isFinite(o.limitPrice) || o.limitPrice < 0 ||
             (o.orderSysId && !o.exchangeId) ||
             (/^[0-6]$/.test(o.combOffsetFlag) &&
              !/^[123567]$/.test(o.combHedgeFlag))) {
          throw new TypeError(`invalid working-order snapshot for ${o.orderRef || "<unknown>"}`);
        }
      }
      const working = active.flatMap((o) => {
        const vol = o.volumeTotalOriginal - o.volumeTraded;
        const supportedSingleLeg = /^[0-6]$/.test(o.combOffsetFlag);
        if (supportedSingleLeg && o.combOffsetFlag !== "0")
          return []; // a representable close order adds no opening exposure
        return [{
            frontId: o.frontId,
            sessionId: o.sessionId,
            orderRef: o.orderRef,
            instrumentId: o.instrumentId,
            isLong: o.direction === "0",
            hedgeFlag: o.combHedgeFlag?.[0] || "1",
            vol,
            price: o.limitPrice,
            traded: o.volumeTraded,
            // Must be byte-identical to the native realtime key built in
            // traderspi.gen.cc (OnRtnOrder/OnRtnTrade):
            // day|broker|investor|exchange|instrument|orderSysId. broker/investor
            // are the confirmed session identity (validated == every row above),
            // so a reservation rebuilt on resync correlates with the subsequent
            // realtime order/trade callbacks instead of being stranded uncertain.
            orderSysKey: o.tradingDay && o.exchangeId && o.orderSysId
              ? `${o.tradingDay}|${brokerId}|${investorId}|${o.exchangeId}|${o.instrumentId}|${o.orderSysId}`
              : "",
            supportedSingleLeg,
          }];
      });
      this.assertRequestContext(contextToken, "order synchronization");
      if (this.native.rebuildReservations(working, revision) as boolean) {
        const unsupported = working.filter((o) => !o.supportedSingleLeg);
        if (unsupported.length > 0) {
          throw new Error(
            `unsupported working order ${unsupported[0].orderRef}; opening inserts are fail-closed`
          );
        }
        return working.length;
      }
      if (attempt < 4) await sleep(100);
    }
    throw new Error("order snapshot could not stabilize; live state kept unchanged");
  }

  /**
   * Arm a latency-critical trigger: when `md` sees the condition, the order is
   * sent from C++ on the callback thread (through this Trader's risk gate),
   * with no JS round trip. One-shot. The acknowledgement arrives via the normal
   * rtn-order / rsp-order-insert events (correlate by the order's orderRef).
   */
  arm(md: MarketData, spec: ArmSpec): ArmHandle {
    // Validate up front: the order fires from C++ with no JS in the loop, so a
    // malformed template cannot reach the callback-thread send path.
    const cleanSpec = copyOwnRecord(spec, "arm spec");
    assertObjectKeys(cleanSpec, "arm spec", ARM_SPEC_KEYS);
    if (!cleanSpec.instrumentId || cleanSpec.instrumentId.includes("\0") ||
        !Number.isFinite(cleanSpec.triggerPrice) || !(cleanSpec.triggerPrice > 0) ||
        cleanSpec.triggerPrice >= 1e300) {
      throw new Error("arm: spec.instrumentId and a positive triggerPrice are required");
    }
    const validatedOrder = this.validateOrder(cleanSpec.order, "arm.order");
    if (cleanSpec.side !== "buy" && cleanSpec.side !== "sell") {
      throw new Error("arm: spec.side must be buy or sell");
    }
    if (validatedOrder.instrumentId !== cleanSpec.instrumentId) {
      throw new Error("arm: spec.instrumentId must match spec.order.instrumentId");
    }
    if ((cleanSpec.side === "buy") !== (validatedOrder.direction === "0")) {
      throw new Error("arm: spec.side must match spec.order.direction");
    }
    const order = this.withAutoOrderRef(validatedOrder);
    const bytes = this.encode(STRUCT_ID.InputOrder, order as Record<string, unknown>);
    md.attachArm(this);
    // The triggering feed is also the authoritative fresh-price source for
    // deviation/notional/margin checks. Without this wiring an armed order can
    // see the trigger tick yet fail every monetary gate as "no reference".
    this.trackMarketData(md);
    const id: number = this.native.arm(
      cleanSpec.instrumentId,
      cleanSpec.side === "buy" ? "0" : "1",
      cleanSpec.triggerPrice,
      bytes
    );
    this.commitOrderRef(order);
    return { id, disarm: () => this.native.disarm(id) as boolean };
  }

  /** Observability for armed triggers (they fire in C++ with no JS in the loop):
   *  how many fired and were sent vs were refused by the risk gate / send.
   *  A blocked armed order is otherwise invisible, so poll this after a trigger
   *  you expected to fire. */
  get armStats(): { fired: number; blocked: number } {
    return { fired: this.native._armFireCount(), blocked: this.native._armBlockedCount() };
  }

  /** @internal used by MarketData.attachArm to share the arm registry. */
  _armRegistry(): unknown {
    return this.native._armRegistry();
  }

  /** @internal test-only: how many times armed triggers have fired (sent). */
  _armFireCount(): number {
    return this.native._armFireCount();
  }
  /** @internal test-only: how many armed triggers were blocked on fire. */
  _armBlockedCount(): number {
    return this.native._armBlockedCount();
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
