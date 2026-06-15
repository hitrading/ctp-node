// Lifecycle / leak regression: close() must free the native event-channel ring
// deterministically (not wait for the GC finalizer). The rings are large
// (~maxStructSize * 8192 MD / 4096 Trader slots) and off-heap, so V8 feels no
// GC pressure from them; before the fix a create/close loop leaked ~32 MB/cycle
// (MD+Trader) even under a forced GC. This runs with NO gc() on purpose: RSS
// must stay flat purely from close() freeing the ring. Also covers
// methods-after-close (must not crash) and a cross-thread burst drain.
import { MarketData, Trader } from "../dist/index.js";
import { createRequire } from "node:module";
import { existsSync, mkdirSync } from "node:fs";

mkdirSync("flow-md", { recursive: true });
mkdirSync("flow-td", { recursive: true });
const require = createRequire(import.meta.url);
const native = require(["../build/Release/ctp.node", "../build/Debug/ctp.node"]
  .find((p) => existsSync(new URL(p, import.meta.url))));

let pass = 0, fail = 0;
const check = (c, m) => (c ? (pass++, console.log("  PASS:", m)) : (fail++, console.error("  FAIL:", m)));
const rss = () => Math.round(process.memoryUsage().rss / 1048576);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ----- deterministic ring free on close() (raw native, NO gc) -----
const before = rss();
for (let i = 0; i < 40; i++) {
  const md = new native.MarketData("./flow-md/", "tcp://127.0.0.1:1");
  md.close();
  const td = new native.Trader("./flow-td/", "tcp://127.0.0.1:1");
  td.close();
}
const after = rss();
// 40 MD(~21 MB) + 40 Trader(~10 MB) rings = ~1.2 GB if leaked; a sane bound is
// well under that. Without the fix this hit ~700 MB by cycle 20.
check(after - before < 120, `no ring leak on close (RSS ${before} -> ${after} MB across 80 close() calls)`);

// ----- methods after close must not crash -----
{
  const td = new Trader("./flow-td/", "tcp://127.0.0.1:1");
  td.setMultiplier("rb2610", 10);
  td._applyTestTrade("rb2610", true, true, 3000, 1);
  td.close();
  let crashed = false;
  try {
    void td.positionCost();
    td.resetPositions();
    void td.getTradingDay();
    const rejected = await td
      .reqOrderInsert({ instrumentId: "rb2610", direction: "0", combOffsetFlag: "0", limitPrice: 3000, volumeTotalOriginal: 1 })
      .then(() => false).catch(() => true);
    check(rejected, "reqOrderInsert after close rejects (api gone)");
  } catch (e) { crashed = true; console.error("    threw:", e.message); }
  check(!crashed, "Trader methods after close do not crash");
}
{
  const md = new MarketData("./flow-md/", "tcp://127.0.0.1:1");
  md.close();
  let crashed = false;
  try {
    check(md.subscribe(["rb2610"]) === -1, "subscribe after close returns -1");
    md._injectTestTick();
    md._burstTicks(10);
    void md.droppedRecords;
  } catch (e) { crashed = true; console.error("    threw:", e.message); }
  check(!crashed, "MarketData methods after close do not crash");
}

// ----- cross-thread burst drain: conservation (received + dropped == pushed) -----
{
  const md = new MarketData("./flow-md/", "tcp://127.0.0.1:1");
  let received = 0;
  md.on("rtn-depth-market-data", () => received++);
  const N = 20000;
  const dropBefore = md.droppedRecords;
  md._burstTicks(N, 20); // background producer thread + coalesced doorbell
  const deadline = Date.now() + 10000;
  while (received + (md.droppedRecords - dropBefore) < N && Date.now() < deadline) await sleep(5);
  for (let i = 0; i < 5; i++) await sleep(5);
  const dropped = md.droppedRecords - dropBefore;
  check(received + dropped === N, `doorbell conservation: received(${received}) + dropped(${dropped}) == ${N}`);
  md.close();
}

// ----- reentrant close() inside a drain handler must not UAF the freed ring -----
// close() frees the native ring synchronously. If a handler invoked from drain()
// calls close() mid-batch (e.g. a kill-switch on a tick), the loop must stop
// reading the now-freed view for the rest of the batch. Before the guard this
// segfaulted (exit 139) whenever records followed the closing one; reaching the
// summary below at all proves the process survived.
{
  const md = new MarketData("./flow-md/", "tcp://127.0.0.1:1");
  let got = 0;
  md.on("rtn-depth-market-data", () => {
    got++;
    if (got === 1) md.close(); // free the ring on record 1; thousands follow in-batch
  });
  md._burstTicks(5000, 0);
  await sleep(300);
  check(got >= 1, `reentrant close() in handler: survived a mid-batch close (delivered ${got}, no UAF)`);
}

console.log(`LIFECYCLE TEST: ${pass} pass, ${fail} fail`);
process.exitCode = fail ? 1 : 0;
