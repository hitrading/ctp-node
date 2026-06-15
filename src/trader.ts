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

const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));

/** Pre-trade risk limits, enforced in C++ before each order is sent. */
export interface RiskConfig {
  /** Max volume per single order. Omit/0 = disabled. */
  maxOrderVolume?: number;
  /** Reject if order price deviates from the reference by > this ratio (e.g.
   *  0.02 = 2%). Feed the reference via trackMarketData() or setRefPrice();
   *  with no reference the check is skipped. */
  maxPriceDeviation?: number;
  /** Max notional (price × volume) per order. Omit/0 = disabled. */
  maxNotional?: number;
  /** Max order sends per second (token bucket). Omit/0 = disabled. */
  maxOrdersPerSec?: number;
  /** Token-bucket burst; defaults to maxOrdersPerSec. */
  orderBurst?: number;
  /** Cap on total open-position cost (Σ open price × volume × multiplier).
   *  0/undefined = disabled. Multipliers and existing positions are picked up
   *  automatically via syncMultipliers() / syncPositions(). */
  maxPositionCost?: number;
}

/** A per-instrument lot cap: a single number caps both sides at that value;
 *  `{ long, short }` caps each side separately (omit a side to leave it open). */
export type LotCap = number | { long?: number; short?: number };

/** Options for the one-call post-connect handshake, {@link Trader.session}. */
export interface SessionOptions {
  brokerId: string;
  userId: string;
  password: string;
  /** Terminal authentication (real accounts require it; SimNow uses
   *  appId "simnow_client_test" / authCode of 16 zeros). Omit both to skip. */
  appId?: string;
  authCode?: string;
  /** Confirm the settlement statement after login — real accounts must do this
   *  before trading (brokers reject orders otherwise). Default true; set false
   *  for environments that don't need it. */
  confirmSettlement?: boolean;
  /** Which risk inputs to fetch after login (default: all). `multipliers` may
   *  be `true` (all instruments) or a list of symbols. */
  sync?: { multipliers?: boolean | string[]; positions?: boolean; orders?: boolean };
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

export class Trader extends TraderBase {
  private broker?: string;
  private investor?: string;
  private orderRefSeq = 0;

  constructor(flowPath: string, fronts: string | string[]) {
    super(new native.Trader(flowPath, fronts), native.__layoutData(), TRADER_EVENTS);
    // Remember credentials from the login response so sync* needs no args, and
    // seed the order-ref counter past the broker's max so auto-assigned refs
    // never collide with a prior session's (CTP rejects duplicate refs).
    this.on("rsp-user-login", (d: unknown) => {
      const r = d as
        | { brokerId?: string; userId?: string; maxOrderRef?: string; frontId?: number; sessionId?: number }
        | undefined;
      if (r) {
        this.broker = r.brokerId;
        this.investor = r.userId;
        const mx = r.maxOrderRef ? parseInt(r.maxOrderRef, 10) : NaN;
        if (Number.isFinite(mx) && mx > this.orderRefSeq) this.orderRefSeq = mx;
        // Publish our session so reservations key by (front, session, ref) and
        // never collide with another terminal's orders on the same account.
        if (typeof r.frontId === "number" && typeof r.sessionId === "number") {
          this.native.setSession(r.frontId, r.sessionId);
        }
      }
    });
    this.start();
  }

  getApiVersion(): string {
    return this.native.getApiVersion();
  }
  getTradingDay(): string {
    return this.native.getTradingDay();
  }

  /**
   * One-call post-connect handshake. Call it from the `front-connected` handler
   * (and again after a reconnect): authenticate (if appId/authCode given) →
   * login → confirm settlement → sync multipliers / positions / orders. Each
   * step just wraps the matching req / sync method, so you can hand-roll your
   * own flow instead when you need finer control. Returns the row counts from
   * the sync steps. Must run on a connected front (queries need the session up).
   */
  async session(opts: SessionOptions): Promise<{ multipliers: number; positions: number; orders: number }> {
    const { brokerId, userId, password, appId, authCode } = opts;
    if (appId !== undefined && authCode !== undefined) {
      await this.reqAuthenticate({ brokerId, userId, appId, authCode });
    }
    await this.reqUserLogin({ brokerId, userId, password });
    if (opts.confirmSettlement ?? true) {
      // idempotent: confirming an already-confirmed statement is fine
      await this.reqSettlementInfoConfirm({ brokerId, investorId: userId });
    }
    const sync = opts.sync ?? {};
    const mul = sync.multipliers ?? true;
    const result = { multipliers: 0, positions: 0, orders: 0 };
    if (mul) result.multipliers = await this.syncMultipliers(Array.isArray(mul) ? mul : undefined);
    if (sync.positions ?? true) result.positions = await this.syncPositions();
    if (sync.orders ?? true) result.orders = await this.syncOrders();
    return result;
  }

  /** Assign a unique numeric OrderRef when the caller left it blank. CTP treats
   *  OrderRef as a per-session sequence and rejects a duplicate or non-numeric
   *  ref with "不允许重复报单", so an unset ref would otherwise collide. */
  private withAutoOrderRef(order: Partial<InputOrder>): Partial<InputOrder> {
    if (order.orderRef === undefined || order.orderRef === "") {
      return { ...order, orderRef: String(++this.orderRefSeq) };
    }
    return order;
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
    return super.reqOrderInsert(this.withAutoOrderRef(req));
  }

  /** Publish pre-trade risk limits to the C++ enforcer (takes effect at once). */
  riskSet(config: RiskConfig): this {
    this.native.riskSet(config);
    return this;
  }
  /** Kill-switch: C++ immediately blocks every position-opening send — regular,
   *  exec, quote, for-quote, option-self-close and comb-action inserts. Cancels
   *  and other *actions* are deliberately left open so you can still pull working
   *  orders while halted. Takes effect at once; reverse with {@link resume}. */
  halt(): this {
    this.native.riskHalt();
    return this;
  }
  /** Release the kill-switch (see {@link halt}). */
  resume(): this {
    this.native.riskResume();
    return this;
  }

  /** Set the reference price for an instrument (used by maxPriceDeviation). */
  setRefPrice(instrumentId: string, price: number): this {
    this.native.setRefPrice(instrumentId, price);
    return this;
  }

  /** Auto-feed last prices from a MarketData feed so the maxPriceDeviation
   *  check has a live reference. */
  trackMarketData(md: MarketData): this {
    md.on("rtn-depth-market-data", (t) =>
      this.native.setRefPrice(t.instrumentId, t.lastPrice)
    );
    return this;
  }

  /** Set a contract's multiplier (合约乘数) for position-cost accounting. */
  setMultiplier(instrumentId: string, multiplier: number): this {
    this.native.setMultiplier(instrumentId, multiplier);
    return this;
  }

  /**
   * Cap the open position (in lots) for one instrument, enforced in C++ on
   * every opening order. Pass a number to cap both sides at once, or
   * `{ long, short }` to cap each side separately (omit a side or pass <= 0 to
   * leave it uncapped). Long and short are always tracked independently.
   *
   * The check is fill-based (counts confirmed position, like maxPositionCost),
   * so rapid bursts of opens can momentarily overshoot before the fills land —
   * size the cap with that in mind.
   *
   *   td.setMaxPosition("rb2610", 100);                 // long<=100 and short<=100
   *   td.setMaxPosition("ru2610", { long: 100, short: 20 });
   */
  setMaxPosition(instrumentId: string, max: LotCap): this {
    if (typeof max === "number") {
      this.native.setMaxPositionVolume(instrumentId, true, max);
      this.native.setMaxPositionVolume(instrumentId, false, max);
    } else {
      if (max.long !== undefined) this.native.setMaxPositionVolume(instrumentId, true, max.long);
      if (max.short !== undefined) this.native.setMaxPositionVolume(instrumentId, false, max.short);
    }
    return this;
  }

  /** Cap several instruments at once, e.g.
   *  `{ rb2610: 100, ru2610: { long: 100, short: 20 } }`. */
  setMaxPositions(limits: Record<string, LotCap>): this {
    for (const id of Object.keys(limits)) this.setMaxPosition(id, limits[id]);
    return this;
  }

  /**
   * Cap one instrument's open-position cost (Σ open price × volume × multiplier,
   * long and short summed - a gross capital/concentration limit), enforced in
   * C++ on every opening order. This is per-instrument and independent of the
   * account-wide cap set via riskSet({ maxPositionCost }); both apply. Pass
   * `maxCost <= 0` to remove it. Fill-based, same caveat as maxPositionCost.
   */
  setMaxPositionCost(instrumentId: string, maxCost: number): this {
    this.native.setMaxInstrumentCost(instrumentId, maxCost);
    return this;
  }

  /** Cap several instruments' open-position cost at once, e.g.
   *  `{ ag2608: 2_000_000, au2608: 5_000_000 }`. */
  setMaxPositionCosts(limits: Record<string, number>): this {
    for (const id of Object.keys(limits)) {
      this.native.setMaxInstrumentCost(id, limits[id]);
    }
    return this;
  }

  /** Seed a pre-existing position's open cost (for the maxPositionCost cap). */
  seedPosition(
    instrumentId: string,
    side: "long" | "short",
    volume: number,
    openCost: number
  ): this {
    this.native.seedPosition(instrumentId, side === "long", volume, openCost);
    return this;
  }

  /** Seed open-position cost from reqQryInvestorPosition() rows (posiDirection
   *  '2' = long, '3' = short). */
  seedFromPositions(
    positions: ReadonlyArray<{
      instrumentId: string;
      posiDirection: string;
      position: number;
      openCost: number;
    }>
  ): this {
    // Aggregate by instrument+side (CTP may return several rows per instrument).
    const agg = new Map<string, { id: string; long: boolean; vol: number; cost: number }>();
    for (const p of positions) {
      if (!(p.position > 0)) continue;
      const long = p.posiDirection === "2";
      const key = `${p.instrumentId}|${long}`;
      const e = agg.get(key) ?? { id: p.instrumentId, long, vol: 0, cost: 0 };
      e.vol += p.position;
      e.cost += p.openCost;
      agg.set(key, e);
    }
    for (const e of agg.values()) this.seedPosition(e.id, e.long ? "long" : "short", e.vol, e.cost);
    return this;
  }

  /** Current total open-position cost tracked by the C++ risk engine. */
  positionCost(): number {
    return this.native.positionCost();
  }

  /** Clear all tracked position cost. */
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
    attempts = 5
  ): Promise<T[]> {
    let lastErr: unknown;
    for (let i = 0; i < attempts; i++) {
      if (i > 0) await sleep(1100); // respect the ~1/sec query limit
      try {
        const rows = await query();
        if (!requireNonEmpty || rows.length > 0) return rows;
      } catch (e) {
        lastErr = e; // flow-control rejection - back off and retry
      }
    }
    if (lastErr) throw lastErr;
    return [];
  }

  /** Fetch contract multipliers from CTP and apply them. No args queries all
   *  instruments (one request); otherwise queries the given symbols. Retries
   *  through cold-start flow control so the multiplier is reliably applied. */
  async syncMultipliers(instrumentIds?: string[]): Promise<number> {
    let count = 0;
    const apply = (rows: unknown[]) => {
      for (const r of rows as Array<{ instrumentId: string; volumeMultiple: number }>) {
        if (r.volumeMultiple > 0) {
          this.setMultiplier(r.instrumentId, r.volumeMultiple);
          count++;
        }
      }
    };
    if (instrumentIds && instrumentIds.length) {
      for (let i = 0; i < instrumentIds.length; i++) {
        apply(await this.queryWithRetry(() => this.reqQryInstrument({ instrumentId: instrumentIds[i] }), true));
        if (i < instrumentIds.length - 1) await sleep(1100); // query flow control
      }
    } else {
      apply(await this.queryWithRetry(() => this.reqQryInstrument({}), true));
    }
    return count;
  }

  /** Seed open-position cost from CTP (reqQryInvestorPosition). Uses the
   *  logged-in account unless brokerId/investorId are supplied. */
  async syncPositions(opts?: { brokerId?: string; investorId?: string }): Promise<number> {
    const brokerId = opts?.brokerId ?? this.broker;
    const investorId = opts?.investorId ?? this.investor;
    const rows = (await this.queryWithRetry(
      () => this.reqQryInvestorPosition({ brokerId, investorId }),
      false
    )) as Array<{
      instrumentId: string;
      posiDirection: string;
      position: number;
      openCost: number;
    }>;
    this.resetPositions();
    this.seedFromPositions(rows);
    return rows.filter((r) => r.position > 0).length;
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
  async syncOrders(opts?: { brokerId?: string; investorId?: string }): Promise<number> {
    const brokerId = opts?.brokerId ?? this.broker;
    const investorId = opts?.investorId ?? this.investor;
    const rows = (await this.queryWithRetry(
      () => this.reqQryOrder({ brokerId, investorId }),
      false
    )) as Array<{
      frontId: number;
      sessionId: number;
      orderRef: string;
      instrumentId: string;
      combOffsetFlag: string;
      direction: string;
      orderStatus: string;
      limitPrice: number;
      volumeTotalOriginal: number;
      volumeTraded: number;
    }>;
    // working = queueing (PartTradedQueueing '1' / NoTradeQueueing '3'); open only
    const working = rows
      .filter((o) => (o.orderStatus === "1" || o.orderStatus === "3") && o.combOffsetFlag?.[0] === "0")
      .map((o) => ({
        frontId: o.frontId,
        sessionId: o.sessionId,
        orderRef: o.orderRef,
        instrumentId: o.instrumentId,
        isLong: o.direction === "0",
        vol: o.volumeTotalOriginal - o.volumeTraded,
        price: o.limitPrice,
      }));
    this.native.rebuildReservations(working);
    return working.length;
  }

  /** @internal test-only: feed a synthetic fill into the position-cost tracker. */
  _applyTestTrade(instrumentId: string, isBuy: boolean, isOpen: boolean, price: number, volume: number): void {
    this.native._applyTestTrade(instrumentId, isBuy, isOpen, price, volume);
  }

  /** @internal test-only: drive the in-flight reservation tracker (OnRtnOrder).
   *  status: '1'/'3' = working (queueing), others (e.g. '0' filled, '5'
   *  cancelled) = terminal. */
  _applyTestOrder(frontId: number, sessionId: number, orderRef: string, instrumentId: string, isOpen: boolean, isLong: boolean, status: string, limitPrice: number, volTotal: number, volTraded: number): void {
    this.native._applyTestOrder(frontId, sessionId, orderRef, instrumentId, isOpen, isLong, status, limitPrice, volTotal, volTraded);
  }

  /**
   * Arm a latency-critical trigger: when `md` sees the condition, the order is
   * sent from C++ on the callback thread (through this Trader's risk gate),
   * with no JS round trip. One-shot. The acknowledgement arrives via the normal
   * rtn-order / rsp-order-insert events (correlate by the order's orderRef).
   */
  arm(md: MarketData, spec: ArmSpec): ArmHandle {
    // Validate up front: the order fires from C++ with no JS in the loop, so a
    // malformed template would silently misfire (e.g. a missing combOffsetFlag
    // defaults to a "close" and skips the position reservation).
    if (!spec.instrumentId || !(spec.triggerPrice > 0)) {
      throw new Error("arm: spec.instrumentId and a positive triggerPrice are required");
    }
    const order = this.withAutoOrderRef(spec.order);
    if (!order.instrumentId || order.direction === undefined || !order.combOffsetFlag ||
        !(Number(order.volumeTotalOriginal) > 0)) {
      throw new Error("arm: spec.order needs instrumentId, direction, combOffsetFlag and volumeTotalOriginal > 0");
    }
    md.attachArm(this);
    const bytes = this.encode(STRUCT_ID.InputOrder, order as Record<string, unknown>);
    const id: number = this.native.arm(
      spec.instrumentId,
      spec.side === "buy" ? "0" : "1",
      spec.triggerPrice,
      bytes
    );
    return { id, disarm: () => this.native.disarm(id) as boolean };
  }

  /** Observability for armed triggers (they fire in C++ with no JS in the loop):
   *  how many fired and were sent vs were refused by the risk gate / send.
   *  A blocked armed order is otherwise invisible, so poll this after a trigger
   *  you expected to fire. */
  armStats(): { fired: number; blocked: number } {
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
  on(event: "rsp-user-login", cb: (data: RspUserLogin, opts: CallbackOptions) => void): this;
  on(event: "err-rtn-order-insert", cb: (data: InputOrder, opts: CallbackOptions) => void): this;
  on(event: string, cb: (...args: never[]) => void): this;
  on(event: string, cb: (...args: never[]) => void): this {
    return super.on(event, cb as (...args: unknown[]) => void);
  }
}

export { TraderEvent };
