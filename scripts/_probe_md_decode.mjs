// TEMP probe: verify every DepthMarketData field decodes from its own layout
// offset/kind with no overlap/mis-map. Place a unique value at each field's
// offset (offsets = compiler truth from __layoutData), decode via the
// production decoder, assert round-trip per field. Also exercise ForQuoteRsp,
// SpecificInstrument, RspUserLogin, UserLogout, MulticastInstrument.
import { createRequire } from "node:module";
import { existsSync } from "node:fs";
import { parseLayouts, buildDecoder } from "../dist/codec.js";
import { STRUCT_ID, STRUCTS } from "../dist/generated/structs.gen.js";

const require = createRequire(import.meta.url);
const cand = ["../build/Release/ctp.node", "../build/Debug/ctp.node"];
const found = cand.find((p) => existsSync(new URL(p, import.meta.url)));
const native = require(found);
const layouts = parseLayouts(native.__layoutData());

function probe(structName) {
  const id = STRUCT_ID[structName];
  const lay = layouts[id];
  const desc = STRUCTS[id];
  const buf = new ArrayBuffer(lay.size);
  const view = new DataView(buf);
  const u8 = new Uint8Array(buf);

  // Assign unique deterministic values per field, written at the field offset.
  const expect = {};
  let strSeq = 0;
  for (let i = 0; i < lay.fields.length; i++) {
    const f = lay.fields[i];
    const js = desc.fields[i].js;
    switch (f.kind) {
      case 0: {
        // ASCII string unique per field, fitting in f.size-1 (leave NUL).
        let s = (js.slice(0, 3) + strSeq).slice(0, Math.max(0, f.size - 1));
        strSeq++;
        for (let k = 0; k < s.length; k++) u8[f.offset + k] = s.charCodeAt(k);
        expect[js] = s;
        break;
      }
      case 1: {
        const v = 100000 + i; // distinct int32
        view.setInt32(f.offset, v, true);
        expect[js] = v;
        break;
      }
      case 2: {
        const v = 1000 + i; // distinct int16
        view.setInt16(f.offset, v, true);
        expect[js] = v;
        break;
      }
      case 3: {
        const v = i + 0.5; // distinct double
        view.setFloat64(f.offset, v, true);
        expect[js] = v;
        break;
      }
    }
  }

  const decode = buildDecoder(id, lay);
  const got = decode(view, u8, 0);

  let fails = 0;
  const keys = new Set(Object.keys(expect));
  // Field-count check
  if (Object.keys(got).length !== lay.fields.length) {
    console.error(`  ${structName}: decoded key count ${Object.keys(got).length} != layout fields ${lay.fields.length}`);
    fails++;
  }
  for (const [k, v] of Object.entries(expect)) {
    if (got[k] !== v) {
      console.error(`  ${structName}.${k}: got ${JSON.stringify(got[k])} expected ${JSON.stringify(v)}`);
      fails++;
    }
  }
  // Detect overlap: any decoded key not in expect, or stale duplicate keys
  for (const k of Object.keys(got)) if (!keys.has(k)) { console.error(`  ${structName}: extra key ${k}`); fails++; }

  // Detect offset overlap explicitly (two fields sharing bytes -> corruption)
  const spans = lay.fields.map((f) => [f.offset, f.offset + f.size]);
  for (let a = 0; a < spans.length; a++)
    for (let b = a + 1; b < spans.length; b++)
      if (spans[a][0] < spans[b][1] && spans[b][0] < spans[a][1]) {
        console.error(`  ${structName}: field ${desc.fields[a].js} and ${desc.fields[b].js} overlap`);
        fails++;
      }

  console.log(`${structName} (id ${id}): ${lay.fields.length} fields, struct size ${lay.size}, ${fails === 0 ? "PASS" : "FAIL(" + fails + ")"}`);
  return fails;
}

let total = 0;
for (const s of ["DepthMarketData", "ForQuoteRsp", "SpecificInstrument", "RspUserLogin", "UserLogout", "MulticastInstrument"]) {
  total += probe(s);
}

// Spot-print the full DepthMarketData decode of the native C++-filled sample to
// eyeball the bid/ask arrays (these are 0 in the sample but offsets are proven
// by the per-field test above + the existing C++ round-trip test).
const id = STRUCT_ID["DepthMarketData"];
const sbuf = native.__sampleDepthMarketData();
const sv = new DataView(sbuf.buffer, sbuf.byteOffset, sbuf.byteLength);
const su8 = new Uint8Array(sbuf.buffer, sbuf.byteOffset, sbuf.byteLength);
const sobj = buildDecoder(id, layouts[id])(sv, su8, 0);
console.log("\nnative sample DepthMarketData decoded keys:", Object.keys(sobj).length);
console.log(JSON.stringify(sobj));

console.log(total === 0 ? "\nALL DECODE PROBES PASS" : `\nDECODE PROBES FAILED: ${total}`);
process.exitCode = total === 0 ? 0 : 1;
