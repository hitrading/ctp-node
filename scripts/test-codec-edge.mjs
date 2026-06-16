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

// encodeStruct: undefined/null skipped; over-long truncated, NUL kept; size===1 single byte.
const buf2 = encodeStruct(layout, STRUCTS[SID].fields, { s: "TOOLONGSTRING", i32: undefined, i16: null });
ok(buf2[7] === 0, "char[8] keeps its NUL terminator when input is over-long");
ok(buf2[6] === 0x47, "char[8] wrote the first 7 bytes of the over-long string");
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
const layouts = parseLayouts(native.__layoutData());
const all = buildAllDecoders(layouts);
ok(all.length === layouts.length && typeof all[34] === "function", "buildAllDecoders built a decoder for every struct");

console.log(`CODEC-EDGE TEST: ${pass} pass, ${fail} fail`);
if (fail) process.exit(1);
