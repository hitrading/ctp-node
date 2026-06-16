/*
 * run.mjs - live SimNow integration test (credential-gated). Validates the WIRE
 * behavior that unit tests cannot: the connect/login handshake, the one-call
 * session() (authenticate -> login -> confirm settlement -> sync multipliers /
 * positions / orders), an account query, and the C++ pre-trade risk gate
 * end-to-end against the REAL CTP API.
 *
 * Safe to run any time: the only orders it sends are deliberately oversized so the
 * pre-trade gate rejects them BEFORE they reach the exchange - nothing ever rests.
 *
 * Credentials come ONLY from the environment (never committed). If CTP_USER /
 * CTP_PASS are unset the suite SKIPS (exit 0), so a CI run without the secret
 * stays green:
 *   CTP_USER, CTP_PASS               required - the SimNow investorId + password
 *   CTP_BROKER    (default 9999)
 *   CTP_APPID     (default simnow_client_test)
 *   CTP_AUTHCODE  (default 16 zeros)
 *   CTP_TD_FRONT  (default the 电信二 trade front; for an off-session nightly set a
 *                  7x24 SimNow front via the CI secret/var)
 */
import { Trader } from "../../dist/index.js";
import { mkdirSync } from "node:fs";

const env = process.env;
if (!env.CTP_USER || !env.CTP_PASS) {
  console.log("[integration] CTP_USER/CTP_PASS not set - skipping live SimNow integration (expected without the secret).");
  process.exit(0);
}
const cfg = {
  brokerId: env.CTP_BROKER || "9999",
  userId: env.CTP_USER,
  password: env.CTP_PASS,
  appId: env.CTP_APPID || "simnow_client_test",
  authCode: env.CTP_AUTHCODE || "0000000000000000",
  tdFront: env.CTP_TD_FRONT || "tcp://182.254.243.31:30002",
};
mkdirSync("flow-int/td", { recursive: true });

let pass = 0, fail = 0;
const ok = (c, m) => { c ? (pass++, console.log("  PASS:", m)) : (fail++, console.log("  FAIL:", m)); };
const deadline = (ms, label) => new Promise((_, rej) => setTimeout(() => rej(new Error("timeout: " + label)), ms));

const td = new Trader("./flow-int/td/", cfg.tdFront);

async function main() {
  // 1) connect
  await Promise.race([new Promise((res) => td.once("front-connected", res)), deadline(20000, "front-connected")]);
  ok(true, "trader front-connected");

  // 2) one-call session handshake (auth -> login -> settlement -> sync)
  const counts = await Promise.race([
    td.session({ brokerId: cfg.brokerId, userId: cfg.userId, password: cfg.password, appId: cfg.appId, authCode: cfg.authCode }),
    deadline(40000, "session"),
  ]);
  ok(counts && typeof counts.multipliers === "number",
     `session() handshake ok (multipliers=${counts?.multipliers}, positions=${counts?.positions}, orders=${counts?.orders})`);

  // 3) account query returns a row (proves req/rsp correlation over the wire)
  const acct = await Promise.race([
    td.reqQryTradingAccount({ brokerId: cfg.brokerId, investorId: cfg.userId }),
    deadline(15000, "qryTradingAccount"),
  ]);
  ok(Array.isArray(acct) && acct.length > 0, `reqQryTradingAccount returned ${Array.isArray(acct) ? acct.length : 0} row(s)`);

  // 4) C++ pre-trade risk gate, end-to-end and LIVE. The order is oversized, so the
  //    gate rejects it before it is ever sent to the exchange (nothing rests).
  td.riskSet({ maxOrderVolume: 1 });
  let blocked = false;
  await td.reqOrderInsert({
    brokerId: cfg.brokerId, investorId: cfg.userId, instrumentId: "rb2510",
    direction: "0", combOffsetFlag: "0", limitPrice: 3000, volumeTotalOriginal: 100,
  }).catch((e) => { blocked = /risk|volume/i.test(String(e && e.message)); });
  ok(blocked, "an oversized order is blocked by the C++ pre-trade risk gate (live, never sent)");
}

main()
  .catch((e) => { fail++; console.log("  FAIL: integration error -", e && e.message); })
  .finally(() => {
    td.close();
    console.log(`INTEGRATION TEST: ${pass} pass, ${fail} fail`);
    process.exit(fail ? 1 : 0);
  });
