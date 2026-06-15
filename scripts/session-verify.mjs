import { Trader } from "../dist/index.js";
import { mkdirSync } from "node:fs";
import process from "node:process";
const e = process.env;
const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a);
mkdirSync("flow-td", { recursive: true });
const td = new Trader("./flow-td/", e.CTP_TD_FRONT);
td.on("front-connected", async () => {
  try {
    const r = await td.session({
      brokerId: e.CTP_BROKER ?? "9999", userId: e.CTP_USER, password: e.CTP_PASS,
      appId: "simnow_client_test", authCode: "0000000000000000",
      confirmSettlement: true,
      sync: { multipliers: ["au2608", "ag2608"], positions: true, orders: true },
    });
    log("session() OK ->", JSON.stringify(r));
    log("getTradingDay:", td.getTradingDay());
  } catch (err) { log("FAILED:", err.message, "errorId=", err.errorId); }
  finally { td.close(); process.exit(0); }
});
log("connecting...");
setTimeout(() => { try { td.close(); } catch {} process.exit(1); }, 20000);
