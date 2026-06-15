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

    if (reqId !== 0) {
      const p = this.pending.get(reqId);
      if (p) {
        if (rspInfo) {
          this.pending.delete(reqId);
          p.reject(
            Object.assign(
              new Error(rspInfo.errorMsg || `CTP error ${rspInfo.errorId}`),
              rspInfo
            )
          );
        } else {
          if (value !== undefined) p.rows.push(value);
          if (isLastRaw === 1) {
            this.pending.delete(reqId);
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
    return encodeStruct(this.layouts[structId], STRUCTS[structId].fields, obj);
  }

  protected nextRequestId(): number {
    return ++this.reqSeq;
  }

  /**
   * Send a request and resolve when its response arrives (matched by id).
   * `single` resolves with the one row; otherwise with the accumulated rows.
   */
  protected request<T>(send: (id: number) => number, single = false): Promise<T> {
    const id = this.nextRequestId();
    return new Promise<T>((resolve, reject) => {
      this.pending.set(id, {
        rows: [],
        resolve: resolve as (v: unknown) => void,
        reject,
        single,
      });
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
        const msg =
          rc === -10001
            ? "blocked by pre-trade risk"
            : rc === -10002
              ? "rate limited (order rate exceeded)"
              : `request rejected by CTP API (code ${rc})`;
        reject(new Error(msg));
      }
    });
  }
}
