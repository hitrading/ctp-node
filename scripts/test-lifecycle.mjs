// Lifecycle / leak regression: close() must free the native event-channel ring
// deterministically (not wait for the GC finalizer). The rings are large
// (~maxStructSize * 8192 MD / 4096 Trader slots) and off-heap, so V8 feels no
// GC pressure from them; before the fix a create/close loop leaked ~32 MB/cycle
// (MD+Trader) even under a forced GC. This runs with NO gc() on purpose: RSS
// must stay flat purely from close() freeing the ring. Also covers
// methods-after-close (must not crash) and a cross-thread burst drain.
import { MarketData, Trader } from "../dist/index.js";
process.env.CTP_NODE_TEST_HOOKS = "1";
import { createRequire } from "node:module";
import { existsSync, mkdirSync } from "node:fs";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

// A vendor DLL stuck in Release() must fail this test, not wedge `npm test`
// forever. The public entry point supervises the real lifecycle workload in a
// disposable process and terminates it on a hard deadline. Running this file
// directly and running it from npm therefore have identical isolation.
if (process.env.CTP_LIFECYCLE_WORKER !== "1") {
  const child = spawn(process.execPath, [fileURLToPath(import.meta.url)], {
    env: { ...process.env, CTP_LIFECYCLE_WORKER: "1", CTP_RECREATE_WARN: "0" },
    stdio: "inherit",
    windowsHide: true,
  });
  const code = await new Promise((resolve) => {
    const timer = setTimeout(() => {
      console.error("LIFECYCLE TEST: FAIL (worker exceeded 60s; terminating vendor DLL process)");
      child.kill("SIGKILL");
      resolve(1);
    }, 60_000);
    child.once("error", (error) => {
      clearTimeout(timer);
      console.error("LIFECYCLE TEST: FAIL (worker spawn)", error);
      resolve(1);
    });
    child.once("exit", (status) => {
      clearTimeout(timer);
      resolve(status ?? 1);
    });
  });
  process.exit(code);
}

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

// _buffer() exposes native ring storage. It must be a single cached external
// ArrayBuffer and close() must detach it before deleting that storage; otherwise
// any retained ArrayBuffer/typed view becomes a deterministic use-after-free.
for (const [label, Ctor, flow] of [
  ["MarketData", native.MarketData, "./flow-md/"],
  ["Trader", native.Trader, "./flow-td/"],
]) {
  const client = new Ctor(flow, "tcp://127.0.0.1:1");
  const first = client._buffer();
  const second = client._buffer();
  const bytes = new Uint8Array(first);
  check(first === second, `${label} _buffer() returns one cached ArrayBuffer`);
  check(first.byteLength > 0 && bytes.byteLength > 0,
    `${label} event-channel buffer is live before close()`);
  client.close();
  check(first.byteLength === 0 && bytes.byteLength === 0,
    `${label} close() detaches retained event-channel buffers`);
}

// ----- methods after close must not crash -----
{
  const td = new Trader("./flow-td/", "tcp://127.0.0.1:1");
  td.close();
  let crashed = false;
  try {
    void td.accountMargin;
    td.resetPositions();
    void td.tradingDay;
    let rejected = false;
    try {
      await td.reqOrderInsert({ instrumentId: "rb2610", direction: "0", combOffsetFlag: "0", limitPrice: 3000, volumeTotalOriginal: 1 });
    } catch {
      // Public validation is intentionally synchronous; after close, either a
      // synchronous validation error or a rejected send Promise is acceptable.
      rejected = true;
    }
    check(rejected, "reqOrderInsert after close rejects safely");
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

// ----- front-address validation: empty fronts throw, never segfault -----
// RegisterFront("") faults deep inside the CTP DLL; the constructor must reject
// empty/empty-list fronts with a clean JS error and skip empties in a mix.
{
  const throwsFront = (fn) => {
    try { const x = fn(); if (x && x.close) x.close(); return false; }
    catch (e) { return /front/i.test(e.message); }
  };
  check(throwsFront(() => new MarketData("./flow-md/", "")), `MD empty front string throws (no segfault)`);
  check(throwsFront(() => new MarketData("./flow-md/", [])), `MD empty front list throws`);
  check(throwsFront(() => new Trader("./flow-td/", ["", ""])), `Trader all-empty fronts throw`);
  check(throwsFront(() => new MarketData("./flow-md/", new Array(1))),
    `MD sparse/inherited front list throws`);
  let mixOk = false;
  try { const md = new MarketData("./flow-md/", ["tcp://127.0.0.1:1", ""]); mixOk = true; md.close(); } catch { /* */ }
  check(mixOk, `valid + empty front: empty skipped, construction succeeds`);
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

// ----- a throwing event handler must NOT wedge / re-deliver / stall the ring -----
// emit() rethrows a listener exception synchronously; before the fix this
// abandoned the drain loop before _release()/readPos advanced, so the poison
// record re-delivered forever and everything behind it stalled (silently, under
// Node's default policy). The fix absorbs the throw per-record (ring still
// advances) and re-surfaces it via the 'error' event / uncaughtException.
{
  const md = new MarketData("./flow-md/", "tcp://127.0.0.1:1");
  const seen = [];
  let surfaced = 0;
  md.on("error", () => surfaced++); // catch the re-surfaced handler errors
  md.on("rtn-depth-market-data", (d) => {
    seen.push(d.volume); // _burstTicks echoes the sequence number in Volume
    if (d.volume === 3) throw new Error("poison handler"); // one bad record
  });
  const N = 30;
  md._burstTicks(N, 0);
  await sleep(400);
  const uniq = new Set(seen);
  const maxSeq = seen.length ? Math.max(...seen) : -1;
  check(seen.length === uniq.size, `throwing handler: no record re-delivered (delivered ${seen.length}, unique ${uniq.size})`);
  check(maxSeq >= N - 1, `throwing handler: ring not wedged, records past the poison (#3) all flow (max seq ${maxSeq}/${N - 1})`);
  check(surfaced >= 1, `throwing handler: error re-surfaced via 'error' event (${surfaced}x)`);
  md.close();
}

console.log(`LIFECYCLE TEST: ${pass} pass, ${fail} fail`);
process.exitCode = fail ? 1 : 0;
