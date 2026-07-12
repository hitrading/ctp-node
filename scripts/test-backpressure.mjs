// Backpressure-policy regression: market data must drop the OLDEST record under
// overflow (keep the freshest quote), NOT the newest. The MD and Trader share
// one EventChannel class; before the per-channel DropPolicy fix it dropped-newest
// for BOTH, so a burst that overflowed the 8192-slot MD ring kept the first 8192
// ticks and discarded every later (fresher) one - exactly wrong for a live quote
// feed. MD now uses safe staging: it logically displaces the oldest unclaimed
// record while claimed slots remain owned by JS until release().
//
// This drives the real cross-thread SPSC + coalesced-doorbell path with a tight
// background producer that laps the JS consumer, and asserts:
//   - overflow actually happened (dropped > 0) so the drop path is exercised;
//   - conservation: received + dropped == produced (nothing miscounted);
//   - keep-newest: the very last tick (seq N-1) is delivered. Under drop-newest
//     this is impossible - once full it refuses everything, so the max delivered
//     seq would be ~numSlots (8191), never N-1;
//   - no double-delivery: each sequence number arrives at most once;
//   - no torn record reaches a handler: _burstTicks stamps the seq in BOTH the
//     header requestId and the payload Volume, written at different instants, so
//     a record overwritten mid-decode would show requestId != volume. Every
//     delivered record must have them equal.
import { MarketData, Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";
process.env.CTP_NODE_TEST_HOOKS = "1";

mkdirSync("flow-md", { recursive: true });
mkdirSync("flow-td-lossless", { recursive: true });

let pass = 0,
  fail = 0;
const check = (c, m) =>
  c ? (pass++, console.log("  PASS:", m)) : (fail++, console.error("  FAIL:", m));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

{
  const md = new MarketData("./flow-md/", "tcp://127.0.0.1:1");
  const seqs = [];
  let controlEvents = 0;
  let controls = 0;
  let mismatched = 0; // delivered records whose header/payload seq disagree (torn)
  md.on("rtn-depth-market-data", (d, opts) => {
    seqs.push(d.volume); // _burstTicks echoes the sequence number in Volume
    // requestId (header) and volume (payload) are written separately; a record
    // torn by a concurrent overwrite would show them disagreeing. seq 0 carries
    // requestId 0 -> reported as undefined, so only cross-check seq > 0.
    if (opts.requestId !== undefined && opts.requestId !== d.volume) mismatched++;
  });
  md.on("heart-beat-warning", (marker) => {
    controlEvents++;
    if (marker === 123) controls++;
  });

  const N = 200000; // >> 8192-slot MD ring -> heavy, sustained overflow
  const dropBefore = md.droppedRecords;
  md._burstTicks(N, 0, true); // include one non-droppable control callback

  const deadline = Date.now() + 20000;
  while (
    (seqs.length + (md.droppedRecords - dropBefore) < N || controls < 1) &&
    Date.now() < deadline
  )
    await sleep(5);
  for (let i = 0; i < 5; i++) await sleep(5); // let the final clean drain settle

  const received = seqs.length;
  const dropped = md.droppedRecords - dropBefore;
  const uniq = new Set(seqs);
  let maxSeq = -1;
  for (const s of seqs) if (s > maxSeq) maxSeq = s; // Math.max(...seqs) would overflow the stack

  check(dropped > 0, `overflow exercised the drop path (dropped ${dropped})`);
  check(
    received + dropped === N,
    `conservation: received(${received}) + dropped(${dropped}) == ${N}`
  );
  check(received < N, `backpressure actually dropped records (received ${received} < ${N})`);
  check(
    maxSeq === N - 1,
    `keep-newest: freshest tick delivered (max seq ${maxSeq} == ${N - 1}); drop-newest could only reach ~8191`
  );
  check(
    received === uniq.size,
    `no double-delivery (received ${received}, unique ${uniq.size})`
  );
  check(mismatched === 0, `no torn record delivered to a handler (${mismatched} header/payload mismatches)`);
  check(
    controlEvents === 1 && controls === 1,
    `non-droppable control callback survives intact (${controlEvents} event(s), ${controls} valid marker(s))`
  );

  md.close();
}

// A control record at the head used to pin every older ring tick: the Oldest
// slow path only inspected the physical ring head and then linearly searched
// staged records. The queue therefore retained stale seq 0..8190 forever while
// replacing only one overflow tick. Produce both bursts before yielding so the
// retained set must be exactly the newest 8192 ticks behind the control.
{
  const md = new MarketData("./flow-md/", "tcp://127.0.0.1:1");
  const seqs = [];
  let controls = 0;
  let forQuotes = 0;
  md.on("rtn-depth-market-data", (d) => seqs.push(d.volume));
  md.on("heart-beat-warning", (marker) => {
    if (marker === 123) controls++;
  });
  md.on("rtn-for-quote", (quote) => {
    if (quote?.instrumentId === "rb2510" && quote?.exchangeId === "SHFE")
      forQuotes++;
  });

  const N = 50000;
  const totalTicks = N + 1; // the control-prefixed burst also contains tick 0
  const dropBefore = md.droppedRecords;
  md._injectTestForQuote();  // business notification before the overflow storm
  md._burstTicks(1, 0, true); // control is physically before its one tick
  md._burstTicks(N, 0);       // joins the first producer, then starts the flood
  md._burstTicks(0, 0);       // join the flood before JS gets a chance to drain

  const deadline = Date.now() + 10000;
  while (
    (seqs.length + (md.droppedRecords - dropBefore) < totalTicks ||
      controls < 1 || forQuotes < 1) &&
    Date.now() < deadline
  )
    await sleep(5);
  const dropped = md.droppedRecords - dropBefore;
  let minSeq = Infinity, maxSeq = -1;
  for (const seq of seqs) {
    if (seq < minSeq) minSeq = seq;
    if (seq > maxSeq) maxSeq = seq;
  }
  check(
    seqs.length === 8192 && dropped === totalTicks - 8192,
    `control-prefixed overflow keeps bounded tick capacity (received ${seqs.length}, dropped ${dropped})`
  );
  check(
    minSeq === N - 8192 && maxSeq === N - 1,
    `control-prefixed overflow retains the newest contiguous window (${minSeq}..${maxSeq})`
  );
  check(controls === 1, `control-prefixed overflow preserves its control record (${controls})`);
  check(forQuotes === 1,
    `control-prefixed overflow preserves its RFQ business notification (${forQuotes})`);
  md.close();
}

// ----- slow consumer widens the torn window (validate() floor regression) -----
// The fast handler above rarely overlaps producer pressure with a decode. A
// handler that busy-waits per record makes the consumer fall far behind a tight
// producer, so it exercises ownership and overflow staging while JS is decoding.
// Every delivered record must still have header requestId == payload Volume.
{
  const md = new MarketData("./flow-md/", "tcp://127.0.0.1:1");
  let torn = 0;
  const seen = new Set();
  let dup = 0, maxSeq = -1, recv = 0;
  const busy = (us) => {
    const end = process.hrtime.bigint() + BigInt(us) * 1000n;
    while (process.hrtime.bigint() < end) { /* spin */ }
  };
  md.on("rtn-depth-market-data", (d, opts) => {
    recv++;
    if (opts.requestId !== undefined && opts.requestId !== d.volume) torn++; // torn slipped through
    if (seen.has(d.volume)) dup++; else seen.add(d.volume);
    if (d.volume > maxSeq) maxSeq = d.volume;
    busy(40); // ~40us/record -> consumer lags, producer laps mid-decode
  });
  const N = 200000;
  const dropBefore = md.droppedRecords;
  md._burstTicks(N, 0);
  const deadline = Date.now() + 20000;
  while (recv + (md.droppedRecords - dropBefore) < N && Date.now() < deadline) await sleep(5);
  for (let i = 0; i < 5; i++) await sleep(5);
  const dropped = md.droppedRecords - dropBefore;
  check(recv + dropped === N, `slow-consumer conservation: received(${recv}) + dropped(${dropped}) == ${N}`);
  check(torn === 0, `slow-consumer: no torn record delivered under heavy lapping (${torn} mismatches over ${recv} delivered)`);
  check(dup === 0, `slow-consumer: no double-delivery (${dup} dups)`);
  check(maxSeq === N - 1, `slow-consumer keep-newest: final tick delivered (${maxSeq} == ${N - 1})`);
  md.close();
}

// Trader callbacks are correctness-critical. Overflowing its 4096-slot ring
// must preserve every record in exact order, not drop the newest callback.
{
  const td = new Trader("./flow-td-lossless/", "tcp://127.0.0.1:1");
  const seen = [];
  td.on("rtn-trade", (seq) => seen.push(seq));
  const N = 30000;
  td.native._burstEvents(N);
  const deadline = Date.now() + 15000;
  while (seen.length < N && Date.now() < deadline) await sleep(5);
  let ordered = seen.length === N;
  for (let i = 0; ordered && i < seen.length; i++) ordered = seen[i] === i;
  check(td.droppedRecords === 0,
    `Trader lossless overflow reports zero drops (${td.droppedRecords})`);
  check(seen.length === N && ordered,
    `Trader lossless overflow delivered ${seen.length}/${N} callbacks in order`);
  td.close();
}

console.log(`BACKPRESSURE TEST: ${pass} pass, ${fail} fail`);
process.exitCode = fail ? 1 : 0;
