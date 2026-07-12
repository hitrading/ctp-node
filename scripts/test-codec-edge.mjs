// Branch/edge coverage for codec.ts: the error throws, every field kind
// (incl. int16 / kind 2), buildAllDecoders over the real layout table, readStr's
// empty / ASCII / gb18030 paths, and encodeStruct's kinds + char-cap handling.
// (Round-trip happy path is in test-codec.mjs; this targets the cold branches.)
import { parseLayouts, readStr, buildDecoder, buildAllDecoders, encodeStruct } from "../dist/codec.js";
import { STRUCTS } from "../dist/generated/structs.gen.js";
import { native } from "../dist/native-binding.js";

let pass = 0, fail = 0;
const ok = (c, m) => { c ? (pass++, console.log("  PASS:", m)) : (fail++, console.log("  FAIL:", m)); };
const throws = (fn, m) => { try { fn(); ok(false, m + " (did not throw)"); } catch { ok(true, m); } };

// A synthetic descriptor exercising every kind: 0 string, 1 int32, 2 int16, 3 double.
const SID = 90000;
STRUCTS[SID] = { name: "CovAll", fields: [
  { js: "s", kind: 0 }, { js: "i32", kind: 1 }, { js: "i16", kind: 2 }, { js: "d", kind: 3 },
] };
const layout = { size: 40, fields: [
  { offset: 0, size: 8, kind: 0 },
  { offset: 8, size: 4, kind: 1 },
  { offset: 12, size: 2, kind: 2 },
  { offset: 16, size: 8, kind: 3 },
] };

// encodeStruct (kinds 0/1/2/3) -> buildDecoder/decode round-trip.
const buf = encodeStruct(layout, STRUCTS[SID].fields, { s: "AB", i32: 7, i16: 300, d: 1.5 });
const obj = buildDecoder(SID, layout)(new DataView(buf.buffer), buf, 0);
ok(obj.s === "AB", "decode string field (kind 0)");
ok(obj.i32 === 7, "decode int32 field (kind 1)");
ok(obj.i16 === 300, "decode int16 field (kind 2)");
ok(obj.d === 1.5, "decode double field (kind 3)");

// Invalid numeric values and over-capacity strings are rejected, never silently
// truncated/coerced into a different wire request.
throws(() => encodeStruct(layout, STRUCTS[SID].fields, { s: "TOOLONGSTRING" }),
  "char[N] rejects an over-capacity encoded string");
throws(() => encodeStruct(layout, STRUCTS[SID].fields, { s: 123 }),
  "char[N] rejects implicit non-string coercion");
throws(() => encodeStruct(layout, STRUCTS[SID].fields, { s: "AB\0CD" }),
  "char[N] rejects an embedded NUL that would change the wire value");
throws(() => encodeStruct(layout, STRUCTS[SID].fields, { i32: 1.5 }),
  "int32 rejects fractional input");
throws(() => encodeStruct(layout, STRUCTS[SID].fields, { i32: "7" }),
  "int32 rejects implicit string coercion");
throws(() => encodeStruct(layout, STRUCTS[SID].fields, { i16: 40000 }),
  "int16 rejects out-of-range input");
throws(() => encodeStruct(layout, STRUCTS[SID].fields, { d: Infinity }),
  "double rejects non-finite input");
throws(() => encodeStruct(layout, STRUCTS[SID].fields.slice(0, 3), {}),
  "encodeStruct rejects descriptor/layout field-count drift");
throws(() => encodeStruct(layout, STRUCTS[SID].fields, []),
  "encodeStruct rejects an array masquerading as a request object");
throws(() => encodeStruct(layout, STRUCTS[SID].fields, Object.create({ i32: 7 })),
  "encodeStruct rejects inherited request fields / non-plain prototypes");
Object.defineProperty(Object.prototype, "i32", { value: 77, configurable: true });
try {
  const cleanPrototype = encodeStruct(layout, STRUCTS[SID].fields, {});
  ok(new DataView(cleanPrototype.buffer).getInt32(8, true) === 0,
    "encodeStruct ignores a polluted Object.prototype field");
} finally {
  delete Object.prototype.i32;
}
const nullPrototypeRequest = Object.assign(Object.create(null), { i32: 9 });
ok(new DataView(encodeStruct(layout, STRUCTS[SID].fields, nullPrototypeRequest).buffer)
  .getInt32(8, true) === 9, "encodeStruct accepts an explicit null-prototype dictionary");
throws(() => encodeStruct(layout, STRUCTS[SID].fields, { [Symbol("hidden")]: 1 }),
  "encodeStruct rejects symbol request fields instead of silently ignoring them");
throws(() => encodeStruct(layout, STRUCTS[SID].fields, { i32: null }),
  "encodeStruct rejects null fields instead of silently omitting them");
throws(() => encodeStruct(layout, STRUCTS[SID].fields, { typoField: 1 }),
  "encodeStruct rejects unknown request fields instead of silently zeroing them");
throws(() => encodeStruct({ size: 4, fields: [{ offset: 0, size: 4, kind: 9 }] },
  [{ js: "x", kind: 9 }], { x: 1 }),
  "encodeStruct throws on an unknown layout kind");
STRUCTS[90001] = { name: "One", fields: [{ js: "c", kind: 0 }] };
const b1 = encodeStruct({ size: 4, fields: [{ offset: 0, size: 1, kind: 0 }] }, STRUCTS[90001].fields, { c: "X" });
ok(b1[0] === 0x58, "size===1 char field writes the single byte (no reserved NUL)");

// buildDecoder throws: field-count mismatch, unknown kind, missing descriptor.
throws(() => buildDecoder(SID, { size: 4, fields: [{ offset: 0, size: 4, kind: 1 }] }), "buildDecoder throws on field-count mismatch");
STRUCTS[90002] = { name: "Bad", fields: [{ js: "x", kind: 9 }] };
throws(() => buildDecoder(90002, { size: 4, fields: [{ offset: 0, size: 4, kind: 9 }] }), "buildDecoder throws on unknown kind");
throws(() => buildDecoder(987654, { size: 0, fields: [] }), "buildDecoder throws when no descriptor exists");

// readStr: empty (first byte NUL), ASCII, gb18030 fallback.
ok(readStr(new Uint8Array([0, 65, 66]), 0, 3) === "", "readStr returns '' when the first byte is NUL");
ok(readStr(new Uint8Array([65, 66, 0]), 0, 3) === "AB", "readStr ASCII fast path");
ok(readStr(new Uint8Array([0xd6, 0xd0, 0]), 0, 3) === "中", "readStr gb18030 fallback (中)");

// buildAllDecoders over the REAL layout table -> builds a decoder for every struct.
const nativeLayouts = native.__layoutData();
throws(() => parseLayouts([]), "parseLayouts rejects a non-Int32Array");
throws(() => parseLayouts(new Int32Array()), "parseLayouts rejects empty data");
const badStructCount = nativeLayouts.slice(); badStructCount[0]--;
throws(() => parseLayouts(badStructCount), "parseLayouts rejects descriptor/layout struct-count drift");
const badHeader = nativeLayouts.slice(); badHeader[1] = 0;
throws(() => parseLayouts(badHeader), "parseLayouts rejects a non-positive struct size");
const badFieldCount = nativeLayouts.slice(); badFieldCount[2]++;
throws(() => parseLayouts(badFieldCount), "parseLayouts rejects descriptor/layout field-count drift");
throws(() => parseLayouts(nativeLayouts.slice(0, nativeLayouts.length - 1)),
  "parseLayouts rejects truncated data");
const trailing = new Int32Array(nativeLayouts.length + 1);
trailing.set(nativeLayouts); trailing[trailing.length - 1] = 123;
throws(() => parseLayouts(trailing), "parseLayouts rejects trailing data");
const badKind = nativeLayouts.slice(); badKind[5] = 9;
throws(() => parseLayouts(badKind), "parseLayouts rejects an unknown/mismatched field kind");
const badOffset = nativeLayouts.slice(); badOffset[3] = -1;
throws(() => parseLayouts(badOffset), "parseLayouts rejects an out-of-bounds field");
const overlapping = nativeLayouts.slice();
let cursor = 1, overlapPatched = false;
for (let structId = 0; structId < overlapping[0]; structId++) {
  const fieldCount = overlapping[cursor + 1];
  if (fieldCount >= 2) {
    const firstOffset = overlapping[cursor + 2];
    overlapping[cursor + 5] = firstOffset; // second field offset
    overlapPatched = true;
    break;
  }
  cursor += 2 + fieldCount * 3;
}
ok(overlapPatched, "test fixture found a multi-field native struct");
throws(() => parseLayouts(overlapping), "parseLayouts rejects overlapping native fields");
const layouts = parseLayouts(nativeLayouts);
const all = buildAllDecoders(layouts);
ok(all.length === layouts.length && typeof all[34] === "function", "buildAllDecoders built a decoder for every struct");

console.log(`CODEC-EDGE TEST: ${pass} pass, ${fail} fail`);
if (fail) process.exit(1);
