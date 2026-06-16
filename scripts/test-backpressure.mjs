// Backpressure-policy regression: market data must drop the OLDEST record under
// overflow (keep the freshest quote), NOT the newest. The MD and Trader share
// one EventChannel class; before the per-channel DropPolicy fix it dropped-newest
// for BOTH, so a burst that overflowed the 8192-slot MD ring kept the first 8192
// ticks and discarded every later (fresher) one - exactly wrong for a live quote
// feed. MD now uses DropPolicy::Oldest: the producer overwrites the oldest unread
// slot and stays wait-free; the consumer detects the overrun at claim() (skip
// lapped records) and validate() (discard any record the producer overwrote while
// it was being decoded), and owns the drop counter - so the SPSC single-writer
// invariant holds (producer writes only writeIdx_, consumer only readIdx_/drops_).
//
// This drives the real cross-thread SPSC + coalesced-doorbell path with a tight
// background producer that laps the JS consumer, and asserts:
//   - overflow actually happened (dropped > 0) so the drop path is exercised;
//   - conservation: received + dropped == produced (nothing miscounted);
//   - keep-newest: the very last tick (seq N-1) is delivered. Under drop-newest
//     this is impossible - once full it refuses everything, so the max delivered
//     seq would be ~numSlots (8191), never N-1;
//   - no double-delivery: each sequence number arrives at most once (a broken
//     validate() that under-counts torn records would re-deliver a seq);
//   - no torn record reaches a handler: _burstTicks stamps the seq in BOTH the
//     header requestId and the payload Volume, written at different instants, so
//     a record overwritten mid-decode would show requestId != volume. Every
//     delivered record must have them equal.
import { MarketData } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-md", { recursive: true });

let pass = 0,
  fail = 0;
const check = (c, m) =>
  c ? (pass++, console.log("  PASS:", m)) : (fail++, console.error("  FAIL:", m));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

{
  const md = new MarketData("./flow-md/", "tcp://127.0.0.1:1");
  const seqs = [];
  let mismatched = 0; // delivered records whose header/payload seq disagree (torn)
  md.on("rtn-depth-market-data", (d, opts) => {
    seqs.push(d.volume); // _burstTicks echoes the sequence number in Volume
    // requestId (header) and volume (payload) are written separately; a record
    // torn by a concurrent overwrite would show them disagreeing. seq 0 carries
    // requestId 0 -> reported as undefined, so only cross-check seq > 0.
    if (opts.requestId !== undefined && opts.requestId !== d.volume) mismatched++;
  });

  const N = 200000; // >> 8192-slot MD ring -> heavy, sustained overflow
  const dropBefore = md.droppedRecords;
  md._burstTicks(N, 0); // tight background producer thread, no jitter -> laps us

  const deadline = Date.now() + 20000;
  while (
    seqs.length + (md.droppedRecords - dropBefore) < N &&
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

  md.close();
}

// ----- slow consumer widens the torn window (validate() floor regression) -----
// The fast handler above rarely overlaps a producer overwrite with a decode, so
// it under-exercises validate(). A handler that busy-waits per record makes the
// consumer fall far behind a tight producer, so the producer laps the ring
// *while records are being decoded* — the exact condition that exposed the
// off-by-one floor (delivering the boundary record whose slot was mid-overwrite).
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
  md.close();
}

console.log(`BACKPRESSURE TEST: ${pass} pass, ${fail} fail`);
process.exitCode = fail ? 1 : 0;
