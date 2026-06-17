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
  _info(): {
    slotSize: number;
    numSlots: number;
    headerSize: number;
    dropOldest: boolean;
  };
  _dropCount(): number;
  // Present only on drop-oldest channels (market data); used by drainOldest().
  _readIndex?(): number;
  _validate?(claimed: number): number;
  close(): void;
}

interface Pending {
  rows: unknown[];
  resolve: (v: unknown) => void;
  reject: (e: unknown) => void;
  single: boolean;
  timer?: ReturnType<typeof setTimeout>;
}

/** One record decoded out of the ring, split from delivery so the drop-oldest
 *  path can decode a whole batch, then discard any torn (overwritten) records,
 *  before any handler runs. */
interface DecodedRecord {
  evt: number;
  value: unknown;
  options: CallbackOptions;
}

// --- Lifecycle tripwire (dev aid) --------------------------------------------
// The CTP vendor DLLs deadlock inside Release() after a handful of
// Init()->Release() cycles in one process - a long-standing limitation of the
// vendor SDK (its api objects are not safe to create/Init/Release repeatedly),
// NOT a bug in this binding. Correct usage is to create each MarketData/Trader
// ONCE and reuse it, handling reconnects in place; a "reconnect by destroy-and-
// recreate" bug instead churns through clients and will eventually wedge the
// whole process. We count clients that were started (Init) and then closed
// (Release) process-wide and warn ONCE past a threshold, so that bug surfaces in
// dev / logs before it hangs in production. This lives only on close()
// (teardown) - never the hot data path - so it costs nothing where latency
// matters. Tune with CTP_RECREATE_WARN (0 disables); the default stays silent
// for any sane app (a few long-lived clients) and for the test suite. See
// docs/native-hooks.md ("Process lifecycle").
function recreateWarnThreshold(raw: string | undefined): number {
  // Default 3: the vendor DLL deadlocks inside Release() after only ~4
  // Init()/Release() cycles, so a higher threshold would never fire before the
  // very hang it warns about. 3 lets the warning print on the close() that
  // precedes the typical wedge, while a create-once app (one or two long-lived
  // clients closed at shutdown) stays under it. 0 disables.
  const DEFAULT = 3;
  if (raw === undefined) return DEFAULT;
  const n = Number(raw);
  return Number.isFinite(n) && n >= 0 ? Math.floor(n) : DEFAULT;
}
const RECREATE_WARN_THRESHOLD = recreateWarnThreshold(process.env.CTP_RECREATE_WARN);
let startedThenClosed = 0;
let recreateWarned = false;
function noteClientReleased(): void {
  startedThenClosed += 1;
  if (
    RECREATE_WARN_THRESHOLD > 0 &&
    !recreateWarned &&
    startedThenClosed > RECREATE_WARN_THRESHOLD
  ) {
    recreateWarned = true;
    console.warn(
      `[ctp-node] ${startedThenClosed} CTP clients have been created and closed ` +
        `in this process. The CTP vendor DLLs deadlock inside Release() after a few ` +
        `Init()/Release() cycles, so a MarketData/Trader must be created ONCE and ` +
        `reused: handle reconnects in place (re-run your front-connected handshake), ` +
        `never destroy and recreate the client. Set CTP_RECREATE_WARN=0 to silence, ` +
        `or to another number to retune. See docs/native-hooks.md (Process lifecycle).`
    );
  }
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
  // Overflow policy of the underlying channel (from _info()). Drop-oldest (market
  // data) takes the buffered drainOldest() path; drop-newest (trader) the inline
  // drain(). See src/native/channel.h.
  private dropOldest = false;
  // Reused scratch holding a whole decoded batch for the drop-oldest path, so we
  // can validate against producer overruns *after* decoding and before delivering
  // (a torn record never reaches a handler). Indices 0..count-1 are live per drain.
  private batch: (DecodedRecord | undefined)[] = [];
  // Set the instant close() begins. close() frees the native ring synchronously,
  // so if an event handler invoked from drain() calls close() mid-batch, the
  // rest of that batch must NOT keep reading the (now freed) ring view. Every
  // drain loop checks this and bails the moment it flips.
  private closing = false;
  // True once start() has run the native Init(). Gates the lifecycle tripwire
  // above: only a started-then-closed client is an Init()/Release() pair (the
  // cycle the vendor DLL deadlocks on); a never-started object closes harmlessly.
  private started = false;
  private readonly pending = new Map<number, Pending>();
  private reqSeq = 0;
  /** Event ids whose requestId-0 response should resolve the oldest pending
   *  request. Needed only for SimNow market-data login/logout, which don't echo
   *  the id; scoping to those events means an unrelated id-0 response
   *  (sub/unsub/error) can't hijack them. The trader echoes ids and leaves this
   *  empty. */
  protected readonly zeroIdFallbackEvents = new Set<number>();
  /** A request that goes idle this long (no response and, for multi-row queries,
   *  no further rows) rejects, so awaiters don't hang forever. The timer resets
   *  on each row, so a large streaming query isn't cut off by total duration. */
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
    this.dropOldest = !!info.dropOldest;
    const ab = this.native._buffer();
    this.view = new DataView(ab);
    this.u8 = new Uint8Array(ab);
    this.native._start(
      this.dropOldest ? () => this.drainOldest() : () => this.drain()
    );
    this.started = true;
  }

  /** Records dropped under backpressure (ring full). Monitor this. */
  get droppedRecords(): number {
    return this.native._dropCount();
  }

  close(): void {
    if (this.closing) return; // idempotent; also stops any in-progress drain
    this.closing = true;
    this.rejectAllPending("client closed");
    if (this.started) noteClientReleased(); // dev tripwire (see top of file)
    this.native.close();
  }

  /** Re-surface an exception thrown by a user event handler without letting it
   *  break the drain loop. By the time this runs (next tick) the ring has
   *  already advanced, so the feed never wedges. If the app subscribes to
   *  'error' the exception is routed there (catchable); otherwise it is rethrown
   *  as an uncaughtException — the normal fate of a throwing EventEmitter
   *  listener, just deferred so the data plane stays consistent. */
  private surfaceHandlerError(err: unknown): void {
    setImmediate(() => {
      if (this.listenerCount("error") > 0) this.emit("error", err);
      else throw err;
    });
  }

  private decoderFor(id: number): Decoder {
    let d = this.decoders[id];
    if (d === undefined) {
      d = buildDecoder(id, this.layouts[id]);
      this.decoders[id] = d;
    }
    return d;
  }

  /** Decode one struct's raw bytes (e.g. a native snapshot) into a plain object,
   *  reusing the lazily-built per-struct decoder — the same codec the ring uses,
   *  so the result is identical to a streamed record. */
  protected decodeStruct(structId: number, bytes: Uint8Array): Record<string, unknown> {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return this.decoderFor(structId)(view, bytes, 0);
  }

  private drain(): void {
    let count = this.native._claim();
    while (count > 0) {
      const numSlots = this.numSlots;
      const slotSize = this.slotSize;
      for (let i = 0; i < count; i++) {
        // A handler in the previous dispatch() may have called close(), which
        // frees the ring; stop before reading freed memory for the next record.
        if (this.closing) return;
        const slot = ((this.readPos + i) % numSlots) * slotSize;
        try {
          this.dispatch(slot);
        } catch (err) {
          // A user event handler threw. Never let it wedge the data plane: the
          // ring MUST keep advancing, else this record re-delivers forever and
          // every record behind it stalls (silently, under Node's default
          // listener-exception policy). Absorb here; re-surface on the next tick.
          this.surfaceHandlerError(err);
        }
      }
      if (this.closing) return;
      this.native._release(count);
      this.readPos += count;
      count = this.native._claim();
    }
  }

  /**
   * Drop-oldest drain (market data). The producer overwrites the oldest unread
   * record when full and never blocks, so the consumer can be lapped mid-read.
   * We therefore decode the whole claimed batch first, then ask the channel how
   * many of the oldest records it overwrote *during* that decode (validate()),
   * discard those (they hold torn bytes), and only then deliver the rest — so a
   * torn record never reaches a handler. The records we do deliver are always the
   * freshest available, and conservation holds (received + dropped == produced).
   */
  private drainOldest(): void {
    let count = this.native._claim();
    while (count > 0) {
      if (this.closing) return;
      // claim() may have advanced readIdx_ past lapped records; resync our slot
      // cursor to the channel's authoritative read index before decoding.
      this.readPos = (this.native._readIndex as () => number)();
      const numSlots = this.numSlots;
      const slotSize = this.slotSize;
      const batch = this.batch;
      for (let i = 0; i < count; i++) {
        const slot = ((this.readPos + i) % numSlots) * slotSize;
        batch[i] = this.decodeSlot(slot);
      }
      // How many leading (oldest) records did the producer overwrite while we
      // were decoding? Those are torn — discard them (validate() counts them as
      // drops). The remainder were provably untouched during decode.
      const torn = (this.native._validate as (n: number) => number)(count);
      for (let i = 0; i < torn; i++) batch[i] = undefined; // release torn refs
      if (this.closing) {
        for (let i = torn; i < count; i++) batch[i] = undefined;
        return;
      }
      for (let i = torn; i < count; i++) {
        const rec = batch[i]!;
        batch[i] = undefined;
        // A handler from an earlier record this batch may have called close(),
        // which frees the ring. The decoded objects are safe (already copied out
        // of the ring), but stop delivering so app semantics match drain().
        if (this.closing) continue;
        try {
          this.deliver(rec);
        } catch (err) {
          this.surfaceHandlerError(err);
        }
      }
      if (this.closing) return;
      this.native._release(count);
      this.readPos += count;
      count = this.native._claim();
    }
  }

  private dispatch(slot: number): void {
    this.deliver(this.decodeSlot(slot));
  }

  /** Decode one slot into a record (pure read of the ring; no side effects). */
  private decodeSlot(slot: number): DecodedRecord {
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
    // sid bounds check (sid < layouts.length): under the drop-oldest path a slot
    // may be overwritten while we decode it (a torn read). The header int32s are
    // normally read old-or-new intact, but DataView.getInt32 atomicity isn't
    // guaranteed, so a torn sid could land out of range; decoderFor() would then
    // build over an undefined layout and throw. The torn record is discarded by
    // validate() afterward, but the throw would escape the decode loop (which is
    // not wrapped) and crash the doorbell callback. Bounding sid keeps decodeSlot
    // total: an out-of-range sid yields an undefined value, harmlessly dropped.
    if (plen > 0 && sid >= 0 && sid < this.layouts.length)
      value = this.decoderFor(sid)(v, u8, base);
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
    return { evt, value, options };
  }

  /** Settle any matching in-flight request, then emit the named event. Side
   *  effects only — reads no ring memory, so it is safe to call after the
   *  producer may have advanced (drainOldest decodes before this runs). */
  private deliver(rec: DecodedRecord): void {
    const { evt, value, options } = rec;
    const reqId = options.requestId ?? 0;
    const rspInfo = options.rspInfo;

    // Responses (Rsp*) carry isLast (0/1 -> boolean); streaming pushes (Rtn*,
    // front, …) use -1 (-> undefined). Correlate a response to its pending
    // request by requestId; only for the configured zeroIdFallbackEvent
    // (MarketData's login) do we resolve a server requestId of 0 against the
    // oldest pending request — SimNow's MD login doesn't echo the id. Scoping to
    // that one event keeps an unrelated id-0 response from hijacking a request.
    if (options.isLast !== undefined && this.pending.size > 0) {
      let key: number | undefined =
        reqId !== 0 && this.pending.has(reqId) ? reqId : undefined;
      if (key === undefined && reqId === 0 && this.zeroIdFallbackEvents.has(evt)) {
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
          if (options.isLast === true) {
            if (p.timer) clearTimeout(p.timer);
            this.pending.delete(key);
            p.resolve(p.single ? p.rows[0] : p.rows);
          } else {
            this.armRequestTimer(key, p); // more rows expected: reset idle timeout
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
    // CTP echoes the requestId as an int32, and dispatch() reads it back via
    // getInt32, so the pending-map key MUST stay within positive int32 or it
    // would stop matching the echoed wire id past 2^31 requests (responses would
    // then only ever reject on the idle timeout). Cycle 1..2^31-1; skip 0, which
    // is the "no id" / zeroIdFallback sentinel. (Reuse can't collide: nothing
    // stays pending across 2^31 requests — the idle timeout is seconds.)
    this.reqSeq = this.reqSeq >= 0x7fffffff ? 1 : this.reqSeq + 1;
    return this.reqSeq;
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
      // Backstop: if the request goes idle (no response / no further rows for a
      // multi-row query), fail rather than hang forever.
      this.armRequestTimer(id, entry);
    });
  }

  /** (Re)arm a pending request's idle timeout. Called at send and reset on each
   *  row, so a long streaming query isn't cut off by total elapsed time. */
  private armRequestTimer(id: number, entry: Pending): void {
    if (this.requestTimeoutMs <= 0) return;
    if (entry.timer) clearTimeout(entry.timer);
    entry.timer = setTimeout(() => {
      if (this.pending.delete(id)) entry.reject(new Error("request timed out"));
    }, this.requestTimeoutMs);
    if (typeof entry.timer.unref === "function") entry.timer.unref();
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
