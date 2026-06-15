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
  /** Reject if order price deviates from reference by > this ratio (needs a
   *  reference price; not yet fed from MD, so currently advisory). */
  maxPriceDeviation?: number;
  /** Max notional (price × volume) per order. Omit/0 = disabled. */
  maxNotional?: number;
  /** Max order sends per second (token bucket). Omit/0 = disabled. */
  maxOrdersPerSec?: number;
  /** Token-bucket burst; defaults to maxOrdersPerSec. */
  orderBurst?: number;
  /** Cap on total open-position cost (Σ open price × volume × multiplier).
   *  0/undefined = disabled. Set contract multipliers via setMultiplier and
   *  seed any pre-existing positions (seedPosition / seedFromPositions). */
  maxPositionCost?: number;
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

  constructor(flowPath: string, fronts: string | string[]) {
    super(new native.Trader(flowPath, fronts), native.__layoutData(), TRADER_EVENTS);
    // Remember credentials from the login response so sync* needs no args.
    this.on("rsp-user-login", (d: unknown) => {
      const r = d as { brokerId?: string; userId?: string } | undefined;
      if (r) {
        this.broker = r.brokerId;
        this.investor = r.userId;
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

  /** Publish pre-trade risk limits to the C++ enforcer (takes effect at once). */
  riskSet(config: RiskConfig): this {
    this.native.riskSet(config);
    return this;
  }
  /** Kill-switch: C++ blocks all order sends immediately. */
  halt(): this {
    this.native.riskHalt();
    return this;
  }
  /** Release the kill-switch. */
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

  /** Fetch contract multipliers from CTP and apply them. No args queries all
   *  instruments (one request); otherwise queries the given symbols. */
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
        apply(await this.reqQryInstrument({ instrumentId: instrumentIds[i] }));
        if (i < instrumentIds.length - 1) await sleep(1100); // query flow control
      }
    } else {
      apply(await this.reqQryInstrument({}));
    }
    return count;
  }

  /** Seed open-position cost from CTP (reqQryInvestorPosition). Uses the
   *  logged-in account unless brokerId/investorId are supplied. */
  async syncPositions(opts?: { brokerId?: string; investorId?: string }): Promise<number> {
    const brokerId = opts?.brokerId ?? this.broker;
    const investorId = opts?.investorId ?? this.investor;
    const rows = (await this.reqQryInvestorPosition({ brokerId, investorId })) as Array<{
      instrumentId: string;
      posiDirection: string;
      position: number;
      openCost: number;
    }>;
    this.resetPositions();
    this.seedFromPositions(rows);
    return rows.filter((r) => r.position > 0).length;
  }

  /** @internal test-only: feed a synthetic fill into the position-cost tracker. */
  _applyTestTrade(instrumentId: string, isBuy: boolean, isOpen: boolean, price: number, volume: number): void {
    this.native._applyTestTrade(instrumentId, isBuy, isOpen, price, volume);
  }

  /**
   * Arm a latency-critical trigger: when `md` sees the condition, the order is
   * sent from C++ on the callback thread (through this Trader's risk gate),
   * with no JS round trip. One-shot. The acknowledgement arrives via the normal
   * rtn-order / rsp-order-insert events (correlate by the order's orderRef).
   */
  arm(md: MarketData, spec: ArmSpec): ArmHandle {
    md.attachArm(this);
    const bytes = this.encode(
      STRUCT_ID.InputOrder,
      spec.order as Record<string, unknown>
    );
    const id: number = this.native.arm(
      spec.instrumentId,
      spec.side === "buy" ? "0" : "1",
      spec.triggerPrice,
      bytes
    );
    return { id, disarm: () => this.native.disarm(id) as boolean };
  }

  /** @internal used by MarketData.attachArm to share the arm registry. */
  _armRegistry(): unknown {
    return this.native._armRegistry();
  }

  /** @internal test-only: how many times armed triggers have fired. */
  _armFireCount(): number {
    return this.native._armFireCount();
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
