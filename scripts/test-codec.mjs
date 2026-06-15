// Round-trip test: C++ fills a DepthMarketData with known values -> raw bytes;
// JS builds a decoder from the native layout blob and decodes them. If offsets
// and kinds agree end-to-end, the decoded plain object matches the inputs.
import { createRequire } from "node:module";
import { existsSync } from "node:fs";
import { parseLayouts, buildDecoder } from "../dist/codec.js";
import { STRUCT_ID } from "../dist/generated/structs.gen.js";

const require = createRequire(import.meta.url);
const candidates = ["../build/Release/ctp.node", "../build/Debug/ctp.node"];
const found = candidates.find((p) => existsSync(new URL(p, import.meta.url)));
if (!found) {
  console.error("ctp.node not found — build first");
  process.exit(1);
}
const native = require(found);

const layouts = parseLayouts(native.__layoutData());
const id = STRUCT_ID["DepthMarketData"];
const decode = buildDecoder(id, layouts[id]);

const buf = native.__sampleDepthMarketData();
const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
const u8 = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
const obj = decode(view, u8, 0);

const expect = {
  tradingDay: "20260615",
  exchangeId: "SHFE",
  instrumentId: "rb2510",
  updateTime: "21:00:00",
  lastPrice: 3500.5,
  bidPrice1: 3499,
  askPrice1: 3501,
  bidVolume1: 10,
  askVolume1: 20,
  volume: 12345,
  updateMillisec: 500,
};

let failed = 0;
for (const [k, v] of Object.entries(expect)) {
  if (obj[k] !== v) {
    failed++;
    console.error(`  FAIL ${k}: got ${JSON.stringify(obj[k])}, expected ${JSON.stringify(v)}`);
  }
}

console.log(`decoded struct id ${id} (DepthMarketData), ${layouts[id].fields.length} fields`);
console.log(`sample: instrumentId=${obj.instrumentId} lastPrice=${obj.lastPrice} bidPrice1=${obj.bidPrice1} volume=${obj.volume}`);
if (failed === 0) console.log("CODEC ROUND-TRIP: PASS (all checked fields match)");
else {
  console.error(`CODEC ROUND-TRIP: FAIL (${failed} mismatches)`);
  process.exit(1);
}
