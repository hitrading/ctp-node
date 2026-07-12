// Coverage for market-data.ts surface that the e2e tests don't hit offline:
// subscribe/unsubscribe (+ for-quote), logout, tradingDay, and attachArm's
// happy / idempotent / second-trader-throws branches. (Constructor, login,
// _injectTestTick, _burstTicks are covered by test-md / test-arm / test-backpressure.)
import { MarketData, Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";
process.env.CTP_NODE_TEST_HOOKS = "1";

mkdirSync("flow-mdu", { recursive: true });
mkdirSync("flow-mdu-td", { recursive: true });
mkdirSync("flow-mdu-td-2", { recursive: true });
mkdirSync("flow-mdu-front-accessor", { recursive: true });
let pass = 0, fail = 0;
const ok = (c, m) => { c ? (pass++, console.log("  PASS:", m)) : (fail++, console.log("  FAIL:", m)); };
const throws = (fn, m) => { try { fn(); ok(false, m + " (did not throw)"); } catch { ok(true, m); } };
// Optional `frag`: when given, require the rejection message to match it, so a
// test that names a specific validation guard can't be satisfied by an unrelated
// offline/network rejection (needed where a guard has no native/codec backstop).
const rejects = async (p, m, frag) => { try { await p; ok(false, m + " (did not reject)"); } catch (e) { const msg = String(e?.message ?? e); const matched = !frag || new RegExp(frag, "i").test(msg); ok(matched, matched ? m : `${m} (wrong error: ${msg})`); } };

const md = new MarketData("./flow-mdu/", "tcp://127.0.0.1:1");
delete process.env.CTP_NODE_TEST_HOOKS;
throws(() => md._injectTestTick(), "queue-producing MD test hooks are disabled by default");
throws(() => md._injectTestForQuote(), "RFQ test hook is disabled by default");
throws(() => md._injectTestLifecycle("login"),
  "MD lifecycle test hook is disabled by default");
throws(() => md._produceTestTicks(1, 1),
  "native SPI producer test hook is disabled by default");
process.env.CTP_NODE_TEST_HOOKS = "1";
ok(typeof md.subscribe(["rb2510"]) === "number", "subscribe returns a CTP send code");
ok(typeof md.unsubscribe(["rb2510"]) === "number", "unsubscribe returns a CTP send code");
ok(typeof md.subscribeForQuote(["rb2510"]) === "number", "subscribeForQuote returns a CTP send code");
ok(typeof md.unsubscribeForQuote(["rb2510"]) === "number", "unsubscribeForQuote returns a CTP send code");
ok(typeof md.apiVersion === "string", "apiVersion property");
ok(typeof md.tradingDay === "string", "tradingDay property");
throws(() => md.subscribe(["rb2510", 1]), "subscribe rejects non-string instrument ids");
throws(() => md.subscribe(null), "subscribe rejects a non-array instrument list");
const sparseInstrumentIds = new Array(1);
throws(() => md.subscribe(sparseInstrumentIds),
  "subscribe rejects sparse instrument-id arrays instead of reading inherited/omitted ids");
let mdAccessorReads = 0;
const accessorInstrumentIds = new Array(1);
Object.defineProperty(accessorInstrumentIds, 0, {
  configurable: true,
  get() { mdAccessorReads++; return "rb2510"; },
});
md.subscribe(accessorInstrumentIds);
ok(mdAccessorReads === 1,
  "subscribe snapshots each accessor-backed instrument id exactly once");
for (const method of ["subscribe", "unsubscribe", "subscribeForQuote", "unsubscribeForQuote"])
  throws(() => md[method]([""]), `${method} rejects an empty instrument id`);
throws(() => md.subscribe(["rb\0evil"]), "subscribe rejects an embedded NUL instrument id");
await rejects(md.login({ brokerId: 1 }), "login rejects non-string fields");
await rejects(md.login(null), "login rejects null instead of sending a zero request");
await rejects(md.logout([]), "logout rejects an array instead of sending a zero request");
await rejects(md.login({ brokerId: "9999\0evil" }), "login rejects embedded NUL fields");
await rejects(md.login({ brokerID: "9999" }), "login rejects unknown/misspelled fields");
await rejects(md.login(Object.create({ brokerId: "9999" })),
  "login rejects inherited fields / non-plain request objects");
await rejects(md.login(new Proxy({}, { ownKeys() { throw new Error("reflection failed"); } })),
  "login normalizes request-reflection failures into a shape rejection");
await rejects(md.logout({ investorId: "x" }), "logout rejects unknown fields");
throws(() => new MarketData("./flow\0evil/", "tcp://127.0.0.1:1"),
  "MarketData rejects an embedded NUL flowPath before entering CTP");
throws(() => new MarketData("./flow-mdu-invalid/", "tcp://127.0.0.1:1\0evil"),
  "MarketData rejects a NUL-bearing front string before entering CTP");
throws(() => new MarketData("./flow-mdu-invalid/", 42),
  "MarketData rejects a non-string/non-array front list before entering CTP");
throws(() => new MarketData("./flow-mdu-invalid/", [42]),
  "MarketData rejects a non-string entry inside a front array");
throws(() => new MarketData("./flow-mdu-invalid/", ["tcp://127.0.0.1:1\0evil"]),
  "MarketData rejects a NUL-bearing entry inside a front array");
let frontAccessorReads = 0;
const accessorFronts = new Array(1);
Object.defineProperty(accessorFronts, 0, {
  configurable: true,
  get() {
    frontAccessorReads++;
    return frontAccessorReads === 1 ? "tcp://127.0.0.1:1" : "tcp://changed.invalid:1";
  },
});
const accessorFrontMd = new MarketData("./flow-mdu-front-accessor/", accessorFronts);
accessorFrontMd.close();
ok(frontAccessorReads === 1,
  "constructor snapshots each accessor-backed front exactly once");

// snapshot / last — the C++ last-value cache is fail-closed outside a concrete
// authenticated MD session. Test hooks drive real SPI lifecycle callbacks.
ok(md.snapshot("rb2510") === null && md.last("rb2510") === 0,
  "snapshot cache starts invalid before MD login");
md._injectTestTick(3499, 3501);
ok(md.snapshot("rb2510") === null,
  "a pre-login tick cannot populate the snapshot cache");
md._injectTestLifecycle("login");
ok(md.snapshot("rb2510") === null,
  "successful login activates an empty epoch, never an old quote");
md._injectTestTick(3499, 3501);
const snap = md.snapshot("rb2510");
ok(snap !== null && Math.abs(snap.lastPrice - 3500.5) < 1e-9, `snapshot(rb2510).lastPrice = ${snap && snap.lastPrice}`);
ok(snap !== null && snap.instrumentId === "rb2510", "snapshot carries the instrumentId");
ok(Math.abs(md.last("rb2510") - 3500.5) < 1e-9, `last(rb2510) = ${md.last("rb2510")}`);
ok(md.snapshot("nope") === null, "snapshot(unknown) -> null");
ok(md.last("nope") === 0, "last(unknown) -> 0");

md._injectTestLifecycle("connected");
md._injectTestTick();
ok(md.snapshot("rb2510") === null,
  "front-connected is pre-authenticated and invalidates/ignores quotes");
md._injectTestLifecycle("login");
md._injectTestTick();
ok(md.snapshot("rb2510") !== null, "fresh tick is visible after relogin");
md._injectTestLifecycle("disconnected");
md._injectTestTick();
ok(md.snapshot("rb2510") === null && md.last("rb2510") === 0,
  "disconnect clears the cache and racing ticks cannot repopulate it");
md._injectTestLifecycle("login-error");
md._injectTestTick();
ok(md.snapshot("rb2510") === null,
  "failed login leaves the snapshot cache invalid");
md._injectTestLifecycle("login");
md._injectTestTick();
md._injectTestLifecycle("logout");
md._injectTestTick();
ok(md.snapshot("rb2510") === null,
  "logout clears the cache and post-logout ticks stay hidden");
md._injectTestLifecycle("login-pending");
md._injectTestLifecycle("abandon");
md._injectTestLifecycle("login");
md._injectTestTick();
ok(md.snapshot("rb2510") === null,
  "an ambiguous timed-out transition rejects a new login before the old final callback");
md._injectTestLifecycle("late-login");
md._injectTestTick();
ok(md.snapshot("rb2510") === null,
  "a late id-0 login success cannot revive an abandoned MD epoch");
md._injectTestLifecycle("login");
md._injectTestTick();
ok(md.snapshot("rb2510") !== null,
  "the ignored final callback resolves ambiguity and permits a fresh login");
throws(() => md._injectTestLifecycle("unknown"),
  "MD lifecycle hook rejects an unknown state");

md._injectTestLifecycle("login");
md._injectTestTick();
// The LVC entry is erased only on the exchange's OnRspUnSubMarketData
// confirmation (callback-driven via finishSubscriptionLocked), NOT synchronously
// when unsubscribe() sends — so the cached tick must still be readable right
// after the call. (The old test only ever hit its refused-send else-branch
// offline, and its accept branch asserted a synchronous erase that never happens.)
const unsubRc = md.unsubscribe(["rb2510"]);
ok(typeof unsubRc === "number" && md.snapshot("rb2510") !== null,
  "unsubscribe does not synchronously clear the snapshot; the erase awaits the OnRspUnSubMarketData confirmation");

// Native background producer: distinct InstrumentID values must traverse the
// real SPI and populate the native cache, with Volume preserving global order.
const producedTicks = [];
md.on("rtn-depth-market-data", (tick) => {
  if (tick.instrumentId?.startsWith("SIM")) producedTicks.push(tick);
});
md._injectTestLifecycle("login");
const instantDropBase = md.droppedRecords;
md._produceTestTicks(512, 17);
let producerDeadline = Date.now() + 5000;
while (producedTicks.length + (md.droppedRecords - instantDropBase) < 512 &&
       Date.now() < producerDeadline)
  await new Promise((resolve) => setTimeout(resolve, 5));
let producerOrdered = producedTicks.length === 512;
for (let i = 0; producerOrdered && i < producedTicks.length; i++)
  producerOrdered = producedTicks[i].volume === i;
ok(producerOrdered && new Set(producedTicks.map((tick) => tick.instrumentId)).size === 17,
  "instant native SPI burst delivered 512 ordered ticks across 17 real cache keys");
ok(md.snapshot("SIM00000000") !== null && md.snapshot("SIM00000016") !== null,
  "native SPI producer populated distinct SnapshotCache entries");

producedTicks.length = 0;
const pacedDropBase = md.droppedRecords;
const pacedStarted = Date.now();
md._produceTestTicks(200, 7, 2000, 20);
producerDeadline = Date.now() + 5000;
while (producedTicks.length + (md.droppedRecords - pacedDropBase) < 200 &&
       Date.now() < producerDeadline)
  await new Promise((resolve) => setTimeout(resolve, 5));
ok(producedTicks.length === 200 && Date.now() - pacedStarted >= 60,
  "paced native SPI producer honors its sustained-rate schedule");
throws(() => md._produceTestTicks(-1, 1),
  "native SPI producer rejects an invalid count");

// login/logout return request Promises (won't resolve offline; the idle timer is
// unref'd so it won't keep the process alive). Swallow the eventual rejection.
const lp = md.login({ brokerId: "9999", userId: "x", password: "y" });
lp.catch(() => {}); // offline: rejects with a CTP send code; we only assert it's a Promise
ok(lp instanceof Promise, "login() returns a Promise");
await rejects(md.logout({ brokerId: "9999", userId: "x" }),
  "overlapping MD login/logout transitions are rejected when id-0 responses cannot be correlated");
await rejects(md.login({ brokerId: "x".repeat(100) }),
  "login rejects an over-capacity GBK credential instead of truncating it");
const lo = md.logout({ brokerId: "9999", userId: "x" });
ok(lo instanceof Promise, "logout() returns a Promise");
await lo.catch(() => {});
await Promise.resolve(); // let the session-transition finalizer run

// Defensive finalizer branches: an old finalizer must not clear a replacement
// transition, and native teardown errors must never strand the JS transition.
const staleFinalizer = md.runSessionTransition(() => Promise.resolve());
const replacementTransition = Promise.resolve();
md.sessionTransition = replacementTransition;
await staleFinalizer;
await Promise.resolve();
ok(md.sessionTransition === replacementTransition,
  "a stale MD session finalizer cannot clear a replacement transition");
md.sessionTransition = undefined;

const realMdNative = md.native;
md.native = new Proxy(realMdNative, {
  get(target, prop) {
    if (prop === "_abandonSessionTransition")
      return () => { throw new Error("simulated native teardown race"); };
    const value = Reflect.get(target, prop, target);
    return typeof value === "function" ? value.bind(target) : value;
  },
});
await md.runSessionTransition(() => Promise.resolve());
await Promise.resolve();
ok(md.sessionTransition === undefined,
  "a native teardown race cannot strand the MD session transition");
md.native = realMdNative;
// The multicast path passes the raw object straight to native (no this.encode
// backstop), so each guard is the SOLE check — assert its specific message so a
// regression isn't masked by the offline send rejection.
await rejects(md.reqQryMulticastInstrument({ topicId: 1.5 }),
  "multicast query rejects a fractional topicId", "int32");
await rejects(md.reqQryMulticastInstrument(null),
  "multicast query rejects null instead of sending a zero request", "plain object");
await rejects(md.reqQryMulticastInstrument({ instrumentId: "rb\0evil" }),
  "multicast query rejects an embedded NUL instrument id", "NUL");
await rejects(md.reqQryMulticastInstrument({ topicID: 1 }),
  "multicast query rejects unknown/misspelled fields", "unknown field");
const multicast = md.reqQryMulticastInstrument({ topicId: 1, instrumentId: "rb2510" });
multicast.catch(() => {});
ok(multicast instanceof Promise, "reqQryMulticastInstrument() returns a Promise");

const td = new Trader("./flow-mdu-td/", "tcp://127.0.0.1:1");
md.attachArm(td); ok(true, "attachArm: happy path");
md.attachArm(td); ok(true, "attachArm: idempotent for the same trader");
const td2 = new Trader("./flow-mdu-td-2/", "tcp://127.0.0.1:1");
throws(() => md.attachArm(td2), "attachArm rejects a second, different live Trader");
td.close();
md.attachArm(td2); ok(true, "attachArm can replace a registry after its old Trader closes");
throws(() => md.attachArm({}), "attachArm rejects a non-Trader argument");
await rejects(md.logout({ brokerId: "x".repeat(100), userId: "u" }), "logout rejects an over-capacity field at the encode stage");
ok(md.shouldTimeoutRequest(1, false) === true, "a non-session-transition MD request always times out");
ok(md.shouldTimeoutRequest(999_999, true) === false, "an unmatched MD session-transition timeout is linearized natively");

md.close(); td2.close(); // release the native doorbell handles so the process exits
throws(() => md.attachArm(td2), "closed clients cannot be attached for armed orders");
ok(md.tradingDay === "", "tradingDay remains a string after close");
console.log(`MD-UNIT TEST: ${pass} pass, ${fail} fail`);
if (fail) process.exit(1);
