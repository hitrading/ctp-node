// Spacing between query sends derived from the limits a front grants.
import { DEFAULT_QUERY_INTERVAL_MS, queryIntervalMs } from "../dist/query-pacing.js";

let pass = 0, fail = 0;
const ok = (c, m) => { c ? (pass++, console.log("  PASS:", m)) : (fail++, console.log("  FAIL:", m)); };
const interval = (qryFreq, ftdPkgFreq) => queryIntervalMs({ qryFreq, ftdPkgFreq });

ok(DEFAULT_QUERY_INTERVAL_MS === 1100, "the default is one query per second with margin");
ok(interval(1, 6) === 1100, "a front granting one query per second keeps the default");
ok(interval(2, 6) === 550, "the query limit applies when it is the tighter one");
ok(interval(10, 10) === 220, "queries take at most half of the packet budget");
ok(interval(10, 7) === 367, "an odd packet budget rounds down before halving");
ok(interval(2147483647, 10000) === 110, "an unlimited front is capped at ten per second");
ok(interval(10, 1) === 1100, "a one-packet budget keeps the default");
ok(interval(0, 0) === 1100 && interval(-1, 10) === 1100 && interval(Number.NaN, 10) === 1100,
  "missing or nonsensical limits keep the default");

console.log(`query pacing: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
