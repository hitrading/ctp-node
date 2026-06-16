/*
 * smoke-cjs.cjs - verify the package works via CommonJS `require()` (the dual-
 * build path). This loads the esbuild-produced dist/index.cjs, which must (a)
 * expose the public API, (b) actually load the native addon through the
 * import.meta->__filename shim, and (c) resolve the bundled CTP DLL (Windows
 * PATH prepend depends on pkgRoot being computed correctly from dist/index.cjs).
 */
const { mkdirSync } = require("node:fs");
const { MarketData, Trader, Direction, OffsetFlag } = require("../dist/index.cjs");

mkdirSync("flow-smoke", { recursive: true });

if (typeof MarketData !== "function") throw new Error("MarketData not exported from CJS build");
if (typeof Trader !== "function") throw new Error("Trader not exported from CJS build");
if (Direction.Buy !== "0") throw new Error("Direction enum missing/wrong: " + Direction.Buy);
if (OffsetFlag.Open === undefined) throw new Error("OffsetFlag enum missing");

const md = new MarketData("./flow-smoke/", "tcp://127.0.0.1:1");
const v = md.getApiVersion(); // forces the native addon (and CTP DLL) to load
md.close();

console.log("CJS smoke OK — require() works; addon loaded; CTP api =", v, "| Direction.Buy =", Direction.Buy);
