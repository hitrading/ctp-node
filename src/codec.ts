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
  const out: StructLayout[] = [];
  let p = 0;
  const n = data[p++];
  for (let i = 0; i < n; i++) {
    const size = data[p++];
    const fc = data[p++];
    const fields: FieldLayout[] = new Array(fc);
    for (let j = 0; j < fc; j++) {
      fields[j] = { offset: data[p++], size: data[p++], kind: data[p++] };
    }
    out.push({ size, fields });
  }
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
  obj: Record<string, unknown>
): Uint8Array {
  const out = new Uint8Array(layout.size);
  const view = new DataView(out.buffer);
  for (let i = 0; i < layout.fields.length; i++) {
    const f = layout.fields[i];
    const v = obj[fields[i].js];
    if (v === undefined || v === null) continue;
    switch (f.kind) {
      case 0: {
        const bytes = utf8Encoder.encode(String(v));
        const cap = f.size === 1 ? 1 : f.size - 1;
        out.set(bytes.subarray(0, Math.min(bytes.length, cap)), f.offset);
        break;
      }
      case 1:
        view.setInt32(f.offset, Number(v) | 0, true);
        break;
      case 2:
        view.setInt16(f.offset, Number(v) | 0, true);
        break;
      case 3:
        view.setFloat64(f.offset, Number(v), true);
        break;
    }
  }
  return out;
}
