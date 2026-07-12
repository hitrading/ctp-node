/*
 * codec.ts — turn the C++ layout blob into fast, plain-object decoders.
 *
 * Offsets/sizes come from native (offsetof/sizeof truth); field names + kinds
 * from the generated descriptors. For each struct we compile a monomorphic
 * literal decoder via `new Function` once, so the hot path is a single fixed-
 * shape object literal (V8 builds these at tens of millions/sec) — no per-field
 * dispatch, no per-tick native calls.
 *
 * Strings decode lazily-friendly with an ASCII fast path and fall back to
 * gb18030 (Node full-ICU) only when high bytes appear — which also sidesteps
 * the old binding's Windows code-page bug.
 */

import { STRUCTS } from "./generated/structs.gen.js";
import { ownStringKeys } from "./validation.js";

export interface FieldLayout {
  offset: number;
  size: number;
  kind: number; // 0 string, 1 int32, 2 int16, 3 double
}
export interface StructLayout {
  size: number;
  fields: FieldLayout[];
}

/** Parse the packed Int32 blob from native `__layoutData()`. */
export function parseLayouts(data: Int32Array): StructLayout[] {
  if (!(data instanceof Int32Array))
    throw new TypeError("codec: layout data must be an Int32Array");
  if (data.length === 0)
    throw new Error("codec: empty layout data");
  const out: StructLayout[] = [];
  let p = 0;
  const next = (label: string): number => {
    if (p >= data.length) throw new Error(`codec: truncated layout data at ${label}`);
    return data[p++];
  };
  const n = next("struct count");
  let descriptorCount = 0;
  while (STRUCTS[descriptorCount]?.id === descriptorCount) descriptorCount++;
  if (n !== descriptorCount)
    throw new Error(`codec: struct-count mismatch (layout ${n} vs descriptor ${descriptorCount})`);
  for (let i = 0; i < n; i++) {
    const size = next(`struct ${i} size`);
    const fc = next(`struct ${i} field count`);
    if (size <= 0 || fc < 0)
      throw new Error(`codec: invalid layout header for struct ${i}`);
    const desc = STRUCTS[i];
    if (!desc || fc !== desc.fields.length)
      throw new Error(`codec: field-count mismatch for struct ${i}`);
    const fields: FieldLayout[] = new Array(fc);
    const occupied: Array<[number, number]> = [];
    for (let j = 0; j < fc; j++) {
      const offset = next(`struct ${i} field ${j} offset`);
      const fieldSize = next(`struct ${i} field ${j} size`);
      const kind = next(`struct ${i} field ${j} kind`);
      const fixedSize = kind === 1 ? 4 : kind === 2 ? 2 : kind === 3 ? 8 : undefined;
      if (offset < 0 || fieldSize <= 0 || offset + fieldSize > size ||
          kind < 0 || kind > 3 || (fixedSize !== undefined && fieldSize !== fixedSize) ||
          kind !== desc.fields[j].kind) {
        throw new Error(`codec: invalid layout for struct ${i} field ${j}`);
      }
      if (occupied.some(([start, end]) => offset < end && start < offset + fieldSize))
        throw new Error(`codec: overlapping layout for struct ${i} field ${j}`);
      occupied.push([offset, offset + fieldSize]);
      fields[j] = { offset, size: fieldSize, kind };
    }
    out.push({ size, fields });
  }
  if (p !== data.length)
    throw new Error(`codec: trailing layout data (${data.length - p} int32 values)`);
  return out;
}

const gbk = new TextDecoder("gb18030");

/** Read a fixed-width C string field; ASCII fast path, gb18030 fallback. */
export function readStr(u8: Uint8Array, off: number, size: number): string {
  let end = off;
  const lim = off + size;
  let ascii = true;
  while (end < lim && u8[end] !== 0) {
    if (u8[end] >= 0x80) ascii = false;
    end++;
  }
  if (end === off) return "";
  if (ascii) {
    let s = "";
    for (let i = off; i < end; i++) s += String.fromCharCode(u8[i]);
    return s;
  }
  return gbk.decode(u8.subarray(off, end));
}

export type Decoder = (
  view: DataView,
  u8: Uint8Array,
  base: number
) => Record<string, unknown>;

/** Compile a plain-object decoder for one struct from its layout. */
export function buildDecoder(structId: number, layout: StructLayout): Decoder {
  const desc = STRUCTS[structId];
  if (!desc) throw new Error(`codec: no descriptor for struct id ${structId}`);
  if (desc.fields.length !== layout.fields.length) {
    throw new Error(
      `codec: field-count mismatch for ${desc.name} ` +
        `(layout ${layout.fields.length} vs descriptor ${desc.fields.length})`
    );
  }

  const parts: string[] = [];
  for (let i = 0; i < layout.fields.length; i++) {
    const f = layout.fields[i];
    const name = JSON.stringify(desc.fields[i].js);
    const o = f.offset;
    switch (f.kind) {
      case 0:
        parts.push(`${name}: readStr(u8, base + ${o}, ${f.size})`);
        break;
      case 1:
        parts.push(`${name}: view.getInt32(base + ${o}, true)`);
        break;
      case 2:
        parts.push(`${name}: view.getInt16(base + ${o}, true)`);
        break;
      case 3:
        parts.push(`${name}: view.getFloat64(base + ${o}, true)`);
        break;
      default:
        throw new Error(`codec: unknown kind ${f.kind} for ${desc.name}`);
    }
  }

  const body = `return function decode(view, u8, base) { return { ${parts.join(
    ", "
  )} }; };`;
  const factory = new Function("readStr", body) as (r: typeof readStr) => Decoder;
  return factory(readStr);
}

/** Build decoders for every struct (lazy callers can also build one at a time). */
export function buildAllDecoders(layouts: StructLayout[]): Decoder[] {
  return layouts.map((l, id) => buildDecoder(id, l));
}

const utf8Encoder = new TextEncoder();

/**
 * Encode a plain object into a struct's raw bytes (reverse of the decoder).
 * Cold path (requests are infrequent vs ticks), so a simple loop is fine.
 * char[N] string fields reserve a null terminator; single-char fields write 1
 * byte. Request fields are ASCII in practice (ids/codes), so UTF-8 == bytes.
 */
export function encodeStruct(
  layout: StructLayout,
  fields: { js: string; kind: number }[],
  obj: Record<string, unknown>,
  encodeStr: (s: string) => Uint8Array = (s) => utf8Encoder.encode(s)
): Uint8Array {
  const objectKeys = ownStringKeys(obj, "codec: request");
  if (fields.length !== layout.fields.length) {
    throw new Error(
      `codec: encode field-count mismatch (${layout.fields.length} layout vs ${fields.length} descriptor)`
    );
  }
  const knownFields = new Set(fields.map((field) => field.js));
  for (const key of objectKeys) {
    if (!knownFields.has(key))
      throw new TypeError(`codec: unknown request field ${key}`);
  }
  const out = new Uint8Array(layout.size);
  const view = new DataView(out.buffer);
  for (let i = 0; i < layout.fields.length; i++) {
    const f = layout.fields[i];
    const fieldName = fields[i].js;
    const v = Object.prototype.hasOwnProperty.call(obj, fieldName)
      ? obj[fieldName]
      : undefined;
    if (v === undefined) continue;
    switch (f.kind) {
      case 0: {
        // CTP char[N] fields hold up to N-1 bytes + a reserved NUL terminator.
        // Reject over-long encoded input: byte truncation can split a GB18030
        // character and silently change an account/password/business field.
        if (typeof v !== "string")
          throw new TypeError(`${fields[i].js} must be a string`);
        if (v.includes("\0"))
          throw new RangeError(`${fields[i].js} must not contain NUL`);
        const bytes = encodeStr(v);
        const cap = f.size === 1 ? 1 : f.size - 1;
        if (bytes.length > cap) {
          throw new RangeError(
            `${fields[i].js} exceeds CTP field capacity (${bytes.length} encoded bytes > ${cap})`
          );
        }
        out.set(bytes, f.offset);
        break;
      }
      case 1: {
        if (typeof v !== "number" || !Number.isInteger(v) ||
            v < -0x80000000 || v > 0x7fffffff)
          throw new RangeError(`${fields[i].js} must be an int32`);
        view.setInt32(f.offset, v, true);
        break;
      }
      case 2: {
        if (typeof v !== "number" || !Number.isInteger(v) ||
            v < -0x8000 || v > 0x7fff)
          throw new RangeError(`${fields[i].js} must be an int16`);
        view.setInt16(f.offset, v, true);
        break;
      }
      case 3: {
        if (typeof v !== "number" || !Number.isFinite(v))
          throw new RangeError(`${fields[i].js} must be finite`);
        view.setFloat64(f.offset, v, true);
        break;
      }
      default:
        throw new Error(`codec: unknown encode kind ${f.kind} for ${fields[i].js}`);
    }
  }
  return out;
}
