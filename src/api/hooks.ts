/*
 * hooks.ts — public TS contract for the native-sinkable hooks.
 *
 * These interfaces are the "thin shell": JS sets the rules and gets notified;
 * the latency/safety-critical enforcement runs in C++ (see src/native/). The
 * runtime wiring lands in a later milestone — these declarations lock the API
 * surface now so the rest of the codebase can be built against it.
 */

/** Hard pre-trade risk limits, enforced in C++ before every order is sent. */
export interface RiskConfig {
  /** Max volume per single order. Omit/0 = disabled. */
  maxOrderVolume?: number;
  /** Reject orders whose price deviates from the reference by more than this
   *  ratio (e.g. 0.02 = 2%). Omit/0 = disabled. */
  maxPriceDeviation?: number;
  /** Max notional (price × volume) per order. Omit/0 = disabled. */
  maxNotional?: number;
  /** Max orders per second (C++ token bucket). Omit/0 = disabled. */
  maxOrdersPerSec?: number;
  /** Token-bucket burst capacity; defaults to `maxOrdersPerSec`. */
  orderBurst?: number;
}

/** Risk + kill-switch control surface (configures the C++ enforcer). */
export interface RiskApi {
  /** Publish new limits to the C++ engine (atomic, takes effect immediately). */
  set(config: RiskConfig): void;
  /** Engage the kill-switch — C++ blocks every send instantly. */
  halt(): void;
  /** Release the kill-switch. */
  resume(): void;
  /** Whether the kill-switch is currently engaged. */
  readonly halted: boolean;
}

/** A latency-critical armed order: evaluated and fired in C++ on the callback
 *  thread the instant its trigger hits — JS is never in the hot path. */
export interface ArmSpec {
  instrumentId: string;
  side: "buy" | "sell";
  /** Buy fires when ask ≤ triggerPrice; sell when bid ≥ triggerPrice. */
  triggerPrice: number;
  volume: number;
  /** Auto-cancel if price moves past this level. Optional. */
  cancelIfWorseThan?: number;
}

/** Notifications are post-hoc and async — latency here does not matter. */
export interface ArmHandle {
  readonly id: number;
  on(event: "fired", cb: (info: { price: number; volume: number }) => void): this;
  on(event: "rejected", cb: (info: { reason: string }) => void): this;
  /** Remove the trigger from the C++ registry. */
  disarm(): void;
}

/** The native-sinkable surface a Trader will expose (wired in a later step). */
export interface NativeHooks {
  readonly risk: RiskApi;
  /** Sink a latency-critical trigger into C++; returns a control handle. */
  arm(spec: ArmSpec): ArmHandle;
}
