// Reproducible offline full-market capacity probe for the ordinary hot path:
// native background producer -> real MdSpi callback -> 18,111-key SnapshotCache
// -> keep-newest EventChannel -> generated 46-field JS decoder -> EventEmitter
// -> a string-keyed 20-tick rolling strategy state for the tick's real
// InstrumentID. No CTP credentials or network are required.
//
// Defaults run three sustained loads plus one simultaneous all-contract burst:
//   CTP_BENCH_SCENARIOS=50000:5000,500000:30000,1000000:3000
//   CTP_BENCH_INSTRUMENTS=18111
//   CTP_BENCH_WINDOW=20
//   CTP_BENCH_NATIVE_BURST=512
//   CTP_BENCH_INSTANT=18111
import { mkdirSync } from "node:fs";
import { availableParallelism, cpus, totalmem } from "node:os";
import { monitorEventLoopDelay, performance } from "node:perf_hooks";

process.env.CTP_NODE_TEST_HOOKS = "1";
const { MarketData } = await import("../dist/index.js");

const integerEnv = (name, fallback, minimum, maximum = Number.MAX_SAFE_INTEGER) => {
  const value = Number(process.env[name] ?? fallback);
  if (!Number.isSafeInteger(value) || value < minimum || value > maximum)
    throw new Error(`${name} must be an integer in ${minimum}..${maximum}`);
  return value;
};

const parseScenarios = (raw) => raw.split(",").map((part) => {
  const match = /^(\d+):(\d+)$/.exec(part.trim());
  if (!match) throw new Error("CTP_BENCH_SCENARIOS must contain rate:milliseconds pairs");
  const rate = Number(match[1]);
  const durationMs = Number(match[2]);
  if (!Number.isSafeInteger(rate) || rate < 1 || rate > 100_000_000 ||
      !Number.isSafeInteger(durationMs) || durationMs < 100 || durationMs > 600_000)
    throw new Error("benchmark scenario rate/duration is out of range");
  const count = Math.round(rate * durationMs / 1000);
  if (count > 100_000_000)
    throw new Error("a benchmark scenario may produce at most 100,000,000 ticks");
  return { rate, durationMs, count };
});

const scenarios = parseScenarios(
  process.env.CTP_BENCH_SCENARIOS ?? "50000:5000,500000:30000,1000000:3000"
);
const instrumentCount = integerEnv("CTP_BENCH_INSTRUMENTS", 18_111, 1, 1_000_000);
const windowSize = integerEnv("CTP_BENCH_WINDOW", 20, 1, 100_000);
const nativeBurstSize = integerEnv("CTP_BENCH_NATIVE_BURST", 512, 1, 1_000_000);
const instantCount = integerEnv("CTP_BENCH_INSTANT", instrumentCount, 1, 100_000_000);
const logicalCpus = availableParallelism();
const mib = (bytes) => bytes / 1024 / 1024;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

mkdirSync("flow-bench-fullmarket", { recursive: true });
const md = new MarketData("./flow-bench-fullmarket/", "tcp://127.0.0.1:1");
md._injectTestLifecycle("login");

// Pre-create the exact application-side cardinality so allocation churn is not
// mistaken for steady-state throughput. The handler still looks each state up
// by tick.instrumentId, including string decode/hash on every tick.
const states = new Map();
for (let i = 0; i < instrumentCount; i++) {
  states.set(`SIM${String(i).padStart(8, "0")}`, {
    values: new Float64Array(windowSize), cursor: 0, count: 0, sum: 0, last: 0,
  });
}

let activeRun;
let checksum = 0;
md.on("rtn-depth-market-data", (tick) => {
  if (!tick.instrumentId?.startsWith("SIM")) return;
  const run = activeRun;
  if (run) {
    run.received++;
    run.minSequence = Math.min(run.minSequence, tick.volume);
    run.maxSequence = Math.max(run.maxSequence, tick.volume);
  }
  const state = states.get(tick.instrumentId);
  if (!state) throw new Error(`unexpected InstrumentID ${tick.instrumentId}`);
  const previous = state.values[state.cursor];
  state.sum += tick.lastPrice - previous;
  state.values[state.cursor] = tick.lastPrice;
  state.cursor = (state.cursor + 1) % windowSize;
  if (state.count < windowSize) state.count++;
  state.last = tick.lastPrice;
  checksum += state.sum / state.count;
});

const runLoad = async ({ label, count, rate, durationMs, burstSize }) => {
  // Starting another producer joins the previous completed native thread. The
  // counter baseline is therefore exact and cannot include an earlier run.
  const dropStart = md.droppedRecords;
  const run = { received: 0, minSequence: Number.POSITIVE_INFINITY, maxSequence: -1 };
  activeRun = run;
  let peakRss = process.memoryUsage().rss;
  let peakHeap = process.memoryUsage().heapUsed;
  const startRss = peakRss;
  const startHeap = peakHeap;
  const sampler = setInterval(() => {
    const memory = process.memoryUsage();
    peakRss = Math.max(peakRss, memory.rss);
    peakHeap = Math.max(peakHeap, memory.heapUsed);
  }, 10);
  sampler.unref?.();
  const delay = monitorEventLoopDelay({ resolution: 10 });
  delay.enable();
  const cpuStart = process.cpuUsage();
  const eluStart = performance.eventLoopUtilization();
  const started = performance.now();

  md._produceTestTicks(count, instrumentCount, rate, burstSize);
  const expectedMs = rate > 0 ? count / rate * 1000 : 0;
  const deadline = performance.now() + Math.max(15_000, expectedMs * 4 + 5_000);
  while (run.received + (md.droppedRecords - dropStart) < count &&
         performance.now() < deadline)
    await sleep(2);
  // Let the final coalesced doorbell and memory sample settle.
  await new Promise((resolve) => setImmediate(resolve));

  const elapsedMs = performance.now() - started;
  const cpu = process.cpuUsage(cpuStart);
  const cpuMs = (cpu.user + cpu.system) / 1000;
  const elu = performance.eventLoopUtilization(eluStart);
  clearInterval(sampler);
  delay.disable();
  activeRun = undefined;
  const memory = process.memoryUsage();
  peakRss = Math.max(peakRss, memory.rss);
  peakHeap = Math.max(peakHeap, memory.heapUsed);
  const dropped = md.droppedRecords - dropStart;
  // Windows process CPU counters are quantized at roughly one scheduler tick;
  // a ~10 ms instantaneous burst can legitimately read as zero. Do not print a
  // bogus percentage for sub-100 ms runs.
  const cpuMeasurementReliable = elapsedMs >= 100;
  const singleCoreCpuPct = cpuMeasurementReliable ? cpuMs / elapsedMs * 100 : null;
  return {
    label,
    requestedRatePerSecond: rate || "instantaneous",
    requestedDurationMs: durationMs,
    nativeBurstSize: burstSize,
    produced: count,
    received: run.received,
    dropped,
    conservation: run.received + dropped === count,
    elapsedMs,
    producedPerSecond: count / (elapsedMs / 1000),
    deliveredPerSecond: run.received / (elapsedMs / 1000),
    minDeliveredSequence: Number.isFinite(run.minSequence) ? run.minSequence : null,
    maxDeliveredSequence: run.maxSequence >= 0 ? run.maxSequence : null,
    singleCoreCpuPct,
    wholeMachineCpuPct: singleCoreCpuPct === null ? null : singleCoreCpuPct / logicalCpus,
    eventLoopUtilizationPct: elu.utilization * 100,
    eventLoopP99Ms: delay.percentile(99) / 1e6,
    startRssMiB: mib(startRss),
    startHeapMiB: mib(startHeap),
    peakRssMiB: mib(peakRss),
    peakHeapMiB: mib(peakHeap),
  };
};

// Warm JIT/decoders and populate all native SnapshotCache keys before measuring.
const warmupCount = Math.max(instrumentCount, 100_000);
const warmup = await runLoad({
  label: "warmup", count: warmupCount, rate: 250_000,
  durationMs: warmupCount / 250, burstSize: nativeBurstSize,
});
if (!warmup.conservation) throw new Error("warmup failed channel conservation");

const sustained = [];
for (const scenario of scenarios) {
  const result = await runLoad({
    label: `sustained-${scenario.rate}`,
    ...scenario,
    burstSize: nativeBurstSize,
  });
  sustained.push(result);
  if (!result.conservation)
    throw new Error(`${result.label} failed channel conservation`);
}
const instantaneous = await runLoad({
  label: `instant-${instantCount}`,
  count: instantCount,
  rate: 0,
  durationMs: null,
  // One native callback storm; SPI and JS still run concurrently on their real
  // separate threads. This directly tests one tick for every subscribed symbol.
  burstSize: instantCount,
});
if (!instantaneous.conservation)
  throw new Error("instantaneous burst failed channel conservation");

const result = {
  node: process.version,
  platform: `${process.platform}-${process.arch}`,
  cpuModel: cpus()[0]?.model ?? "unknown",
  logicalCpus,
  totalMemoryGiB: totalmem() / 1024 / 1024 / 1024,
  instruments: instrumentCount,
  rollingWindow: windowSize,
  nativeSnapshotKeysModeled: instrumentCount,
  sustained,
  instantaneous,
  checksum,
};
console.log(JSON.stringify(result, null, 2));

md.close();
if ([...sustained, instantaneous].some((row) => !row.conservation))
  process.exitCode = 1;
