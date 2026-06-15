/*
 * client.ts - shared base for MarketData and Trader.
 *
 * Owns the JS side of the data plane: on each coalesced doorbell it drains the
 * whole ring, decodes each record straight from the zero-copy buffer into a
 * plain object, and dispatches it - emitting a named event AND, when the record
 * is a response to an in-flight request (matched by requestId), settling its
 * Promise (accumulating multi-row query responses until isLast).
 *
 * Slot header offsets must match src/native/channel.h.
 */

import { EventEmitter } from "node:events";
import {
  parseLayouts,
  buildDecoder,
  encodeStruct,
  readStr,
  type Decoder,
  type StructLayout,
} from "./codec.js";
import { STRUCTS } from "./generated/structs.gen.js";
import { native as addon } from "./native-binding.js";

/** Encode a request string field: ASCII fast path; GB18030 (CTP's wire charset,
 *  via native) for the rare non-ASCII field. */
function encodeReqString(s: string): Uint8Array {
  for (let i = 0; i < s.length; i++) {
    if (s.charCodeAt(i) > 0x7f) return new Uint8Array(addon.__gbkEncode(s));
  }
  const a = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) a[i] = s.charCodeAt(i);
  return a;
}

const S_EVENT = 0;
const S_REQID = 4;
const S_ISLAST = 8;
const S_ERRID = 12;
const S_SID = 16;
const S_PLEN = 20;
const S_ERRMSG = 24;

export interface RspInfo {
  errorId: number;
  errorMsg: string;
}

export interface CallbackOptions {
  /** Request id this record answers (for req/rsp correlation). */
  requestId?: number;
  /** Whether this is the last record of the response. */
  isLast?: boolean;
  /** Present only when CTP reported an error (errorId !== 0). */
  rspInfo?: RspInfo;
}

export interface NativeClient {
  _start(drain: () => void): void;
  _buffer(): ArrayBuffer;
  _claim(): number;
  _release(n: number): void;
  _info(): { slotSize: number; numSlots: number; headerSize: number };
  _dropCount(): number;
  close(): void;
}

interface Pending {
  rows: unknown[];
  resolve: (v: unknown) => void;
  reject: (e: unknown) => void;
  single: boolean;
  timer?: ReturnType<typeof setTimeout>;
}

export abstract class CtpClient extends EventEmitter {
  // Inherently dynamic N-API object; the NativeClient interface documents the
  // contract the data plane relies on.
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  protected readonly native: any;
  private readonly layouts: StructLayout[];
  private readonly decoders: (Decoder | undefined)[];
  private readonly events: Map<number, string>;
  private view!: DataView;
  private u8!: Uint8Array;
  private slotSize = 0;
  private numSlots = 0;
  private headerSize = 0;
  private readPos = 0;
  private readonly pending = new Map<number, Pending>();
  private reqSeq = 0;
  /** Resolve a response whose server requestId is 0 against the oldest pending
   *  request. Needed only for SimNow market-data login (it doesn't echo the id);
   *  the trader echoes ids, so leaving this off avoids cross-delivering an
   *  unrelated id-0 response to a pending query. MarketData opts in. */
  protected zeroIdFallback = false;
  /** A request whose response never arrives (front dropped a reply, silent flow
   *  control) rejects after this many ms, so awaiters don't hang forever. */
  protected requestTimeoutMs = 30000;

  protected constructor(
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    native: any,
    layoutBlob: Int32Array,
    events: Map<number, string>
  ) {
    super();
    this.setMaxListeners(0);
    this.native = native;
    this.layouts = parseLayouts(layoutBlob);
    this.decoders = new Array(this.layouts.length);
    this.events = events;
    // A dropped connection won't deliver in-flight responses - fail them fast.
    this.on("front-disconnected", () => this.rejectAllPending("front disconnected"));
  }

  /** Reject and clear every in-flight request (on disconnect / close). */
  private rejectAllPending(reason: string): void {
    if (this.pending.size === 0) return;
    const err = new Error(reason);
    for (const [id, p] of this.pending) {
      if (p.timer) clearTimeout(p.timer);
      this.pending.delete(id);
      p.reject(err);
    }
  }

  /** Wire up the ring view + doorbell. Subclasses call this in their ctor. */
  protected start(): void {
    const info = this.native._info();
    this.slotSize = info.slotSize;
    this.numSlots = info.numSlots;
    this.headerSize = info.headerSize;
    const ab = this.native._buffer();
    this.view = new DataView(ab);
    this.u8 = new Uint8Array(ab);
    this.native._start(() => this.drain());
  }

  /** Records dropped under backpressure (ring full). Monitor this. */
  get droppedRecords(): number {
    return this.native._dropCount();
  }

  close(): void {
    this.rejectAllPending("client closed");
    this.native.close();
  }

  private decoderFor(id: number): Decoder {
    let d = this.decoders[id];
    if (d === undefined) {
      d = buildDecoder(id, this.layouts[id]);
      this.decoders[id] = d;
    }
    return d;
  }

  private drain(): void {
    let count = this.native._claim();
    while (count > 0) {
      const numSlots = this.numSlots;
      const slotSize = this.slotSize;
      for (let i = 0; i < count; i++) {
        const slot = ((this.readPos + i) % numSlots) * slotSize;
        this.dispatch(slot);
      }
      this.native._release(count);
      this.readPos += count;
      count = this.native._claim();
    }
  }

  private dispatch(slot: number): void {
    const v = this.view;
    const u8 = this.u8;
    const evt = v.getInt32(slot + S_EVENT, true);
    const reqId = v.getInt32(slot + S_REQID, true);
    const isLastRaw = v.getInt32(slot + S_ISLAST, true);
    const errId = v.getInt32(slot + S_ERRID, true);
    const sid = v.getInt32(slot + S_SID, true);
    const plen = v.getInt32(slot + S_PLEN, true);
    const base = slot + this.headerSize;

    let value: unknown;
    if (plen > 0 && sid >= 0) value = this.decoderFor(sid)(v, u8, base);
    else if (sid === -2 && plen >= 4) value = v.getInt32(base, true);
    else value = undefined;

    const rspInfo: RspInfo | undefined =
      errId !== 0
        ? { errorId: errId, errorMsg: readStr(u8, slot + S_ERRMSG, 81) }
        : undefined;

    const options: CallbackOptions = {
      requestId: reqId !== 0 ? reqId : undefined,
      isLast: isLastRaw === -1 ? undefined : isLastRaw === 1,
      rspInfo,
    };

    // Responses (Rsp*) carry isLast (0/1); streaming pushes (Rtn*, front, …)
    // use -1. Correlate a response to its pending request by requestId. Only
    // when zeroIdFallback is enabled (MarketData) do we fall back to the oldest
    // pending request for a server requestId of 0 — SimNow's market-data login
    // doesn't echo it. The trader echoes ids, so it leaves the fallback off to
    // avoid attaching an unrelated id-0 response to a pending query.
    if (isLastRaw !== -1 && this.pending.size > 0) {
      let key: number | undefined =
        reqId !== 0 && this.pending.has(reqId) ? reqId : undefined;
      if (key === undefined && reqId === 0 && this.zeroIdFallback) {
        key = this.pending.keys().next().value;
      }
      const p = key !== undefined ? this.pending.get(key) : undefined;
      if (p && key !== undefined) {
        if (rspInfo) {
          if (p.timer) clearTimeout(p.timer);
          this.pending.delete(key);
          p.reject(
            Object.assign(
              new Error(rspInfo.errorMsg || `CTP error ${rspInfo.errorId}`),
              rspInfo
            )
          );
        } else {
          if (value !== undefined) p.rows.push(value);
          if (isLastRaw === 1) {
            if (p.timer) clearTimeout(p.timer);
            this.pending.delete(key);
            p.resolve(p.single ? p.rows[0] : p.rows);
          }
        }
      }
    }

    const name = this.events.get(evt);
    this.emit(name ?? `event:${evt}`, value, options);
  }

  /** Encode a plain object into a struct's raw bytes (for req* encoders). */
  protected encode(structId: number, obj: Record<string, unknown>): Uint8Array {
    return encodeStruct(
      this.layouts[structId],
      STRUCTS[structId].fields,
      obj,
      encodeReqString
    );
  }

  protected nextRequestId(): number {
    return ++this.reqSeq;
  }

  /** Message for a non-zero send code. Risk sentinels get a specific message;
   *  the generic pre-trade-risk block also appends the C++ reason (e.g. which
   *  limit) so logs say *why*. Returns undefined for non-risk codes. */
  private riskRejectMessage(rc: number): string | undefined {
    switch (rc) {
      case -10001: {
        // eslint-disable-next-line @typescript-eslint/no-unsafe-member-access
        const reason =
          typeof this.native.lastRiskReason === "function" ? this.native.lastRiskReason() : "";
        return reason ? `blocked by pre-trade risk: ${reason}` : "blocked by pre-trade risk";
      }
      case -10002:
        return "rate limited (order rate exceeded)";
      case -10003:
        return "blocked by max position cost";
      case -10004:
        return "blocked by max position volume (lots)";
      default:
        return undefined;
    }
  }

  /**
   * Send a request and resolve when its response arrives (matched by id).
   * `single` resolves with the one row; otherwise with the accumulated rows.
   */
  protected request<T>(send: (id: number) => number, single = false): Promise<T> {
    const id = this.nextRequestId();
    return new Promise<T>((resolve, reject) => {
      const entry: Pending = {
        rows: [],
        resolve: resolve as (v: unknown) => void,
        reject,
        single,
      };
      this.pending.set(id, entry);
      let rc: number;
      try {
        rc = send(id);
      } catch (e) {
        this.pending.delete(id);
        reject(e);
        return;
      }
      if (rc !== 0) {
        this.pending.delete(id);
        const msg = this.riskRejectMessage(rc) ?? `request rejected by CTP API (code ${rc})`;
        reject(new Error(msg));
        return;
      }
      // Backstop: if no response ever arrives, fail rather than hang forever.
      if (this.requestTimeoutMs > 0) {
        entry.timer = setTimeout(() => {
          if (this.pending.delete(id)) reject(new Error("request timed out"));
        }, this.requestTimeoutMs);
        if (typeof entry.timer.unref === "function") entry.timer.unref();
      }
    });
  }

  /**
   * Submit a fire-and-forget order/action. Exchange-bound inserts get no
   * success response from CTP (only a rejection), so this resolves as soon as
   * CTP accepts the send and rejects if the send is refused. Track the order's
   * actual lifecycle (accepted / filled / cancelled) via the rtn-order and
   * rtn-trade events, correlating by orderRef.
   */
  protected submit(send: (id: number) => number): Promise<void> {
    const id = this.nextRequestId();
    let rc: number;
    try {
      rc = send(id);
    } catch (e) {
      return Promise.reject(e);
    }
    if (rc === 0) return Promise.resolve();
    const msg = this.riskRejectMessage(rc) ?? `order rejected by CTP API (code ${rc})`;
    return Promise.reject(new Error(msg));
  }
}
