// Smoke test: load the freshly built native addon and print the CTP API
// versions. No network needed — this only validates the build/load chain.
import { createRequire } from "node:module";
import { existsSync } from "node:fs";

const require = createRequire(import.meta.url);

const candidates = [
  "../build/Release/ctp.node",
  "../build/Debug/ctp.node",
];

const found = candidates.find((p) => existsSync(new URL(p, import.meta.url)));
if (!found) {
  console.error("ctp.node not found — run `npm run build:native` first");
  process.exit(1);
}

const native = require(found);
console.log("loaded:", found);
console.log("MarketData API version:", native.mdApiVersion());
console.log("Trader API version:    ", native.traderApiVersion());
console.log("\nnative hook self-test (risk / rate-limit / arm):");
console.log(JSON.stringify(native.__riskSelfTest(), null, 2));
console.log("\nring self-test (SPSC claim/release/drop):");
console.log(JSON.stringify(native.__ringSelfTest(), null, 2));
