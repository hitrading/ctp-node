// Micro-benchmark: raw decode-to-plain-object throughput for DepthMarketData.
// This is the hot-path cost that decides whether JS can keep up with the
// open-auction flood. Real CTP load is ~thousands/sec steady, tens of
// thousands/sec in bursts.
import { createRequire } from "node:module";
import { existsSync } from "node:fs";
import { parseLayouts, buildDecoder } from "../dist/codec.js";
import { STRUCT_ID } from "../dist/generated/structs.gen.js";

const require = createRequire(import.meta.url);
const found = ["../build/Release/ctp.node", "../build/Debug/ctp.node"].find((p) =>
  existsSync(new URL(p, import.meta.url))
);
const native = require(found);

const layouts = parseLayouts(native.__layoutData());
const id = STRUCT_ID.DepthMarketData;
const decode = buildDecoder(id, layouts[id]);
const buf = native.__sampleDepthMarketData();
const view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
const u8 = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);

for (let i = 0; i < 200_000; i++) decode(view, u8, 0); // warm up JIT

const N = 5_000_000;
let sink = 0;
const t0 = process.hrtime.bigint();
for (let i = 0; i < N; i++) {
  const o = decode(view, u8, 0);
  sink += o.lastPrice + o.bidPrice1;
}
const t1 = process.hrtime.bigint();

const ms = Number(t1 - t0) / 1e6;
const perSec = N / (ms / 1000);
console.log(`decode DepthMarketData (46 fields -> plain object):`);
console.log(`  ${N.toLocaleString()} ops in ${ms.toFixed(0)} ms = ${(perSec / 1e6).toFixed(2)} M/sec`);
console.log(`  ~${(1e9 / perSec).toFixed(0)} ns per tick`);
console.log(`  headroom vs 50,000 ticks/sec burst: ${Math.round(perSec / 50000)}x`);
if (!Number.isFinite(sink)) process.exitCode = 1;
