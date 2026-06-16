// Coverage for market-data.ts surface that the e2e tests don't hit offline:
// subscribe/unsubscribe (+ for-quote), logout, getTradingDay, and attachArm's
// happy / idempotent / second-trader-throws branches. (Constructor, login,
// _injectTestTick, _burstTicks are covered by test-md / test-arm / test-backpressure.)
import { MarketData, Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-mdu", { recursive: true });
mkdirSync("flow-mdu-td", { recursive: true });
let pass = 0, fail = 0;
const ok = (c, m) => { c ? (pass++, console.log("  PASS:", m)) : (fail++, console.log("  FAIL:", m)); };
const throws = (fn, m) => { try { fn(); ok(false, m + " (did not throw)"); } catch { ok(true, m); } };

const md = new MarketData("./flow-mdu/", "tcp://127.0.0.1:1");
ok(typeof md.subscribe(["rb2510"]) === "number", "subscribe returns a CTP send code");
ok(typeof md.unsubscribe(["rb2510"]) === "number", "unsubscribe returns a CTP send code");
ok(typeof md.subscribeForQuote(["rb2510"]) === "number", "subscribeForQuote returns a CTP send code");
ok(typeof md.unsubscribeForQuote(["rb2510"]) === "number", "unsubscribeForQuote returns a CTP send code");
ok(typeof md.getApiVersion() === "string", "getApiVersion");
ok(typeof md.getTradingDay() === "string", "getTradingDay");

// login/logout return request Promises (won't resolve offline; the idle timer is
// unref'd so it won't keep the process alive). Swallow the eventual rejection.
const lp = md.login({ brokerId: "9999", userId: "x", password: "y" });
lp.catch(() => {}); // offline: rejects with a CTP send code; we only assert it's a Promise
ok(lp instanceof Promise, "login() returns a Promise");
const lo = md.logout({ brokerId: "9999", userId: "x" });
lo.catch(() => {});
ok(lo instanceof Promise, "logout() returns a Promise");

const td = new Trader("./flow-mdu-td/", "tcp://127.0.0.1:1");
md.attachArm(td); ok(true, "attachArm: happy path");
md.attachArm(td); ok(true, "attachArm: idempotent for the same trader");
throws(() => md.attachArm({}), "attachArm throws on a second, different trader");

md.close(); td.close(); // release the native doorbell handles so the process exits
console.log(`MD-UNIT TEST: ${pass} pass, ${fail} fail`);
if (fail) process.exit(1);
