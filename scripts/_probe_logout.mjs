// TEMP probe: drive the REAL CtpClient.dispatch + request() correlation with
// synthetic response slots. We use the real request() with a send() that just
// returns 0 (success), so a real Pending entry is created and keyed by the real
// reqSeq id, then we write a response slot into the real ring ArrayBuffer and
// call the real (private) dispatch(slot). This isolates the id-0 correlation
// logic (the bug surface) from the offline network send.
import { MarketData } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-test", { recursive: true });
const md = new MarketData("./flow-test/", "tcp://127.0.0.1:1");

const S_EVENT = 0, S_REQID = 4, S_ISLAST = 8, S_ERRID = 12, S_SID = 16, S_PLEN = 20, S_ERRMSG = 24;
const MD_BASE = 0x1000;
const SID_RspUserLogin = 2, SID_UserLogout = 3;

const info = md.native._info();
const ab = md.native._buffer();
const view = new DataView(ab);
const u8 = new Uint8Array(ab);

function writeSlot(slotIdx, { evt, reqId, isLast, errId, sid, payloadLen }) {
  const base = slotIdx * info.slotSize;
  view.setInt32(base + S_EVENT, evt, true);
  view.setInt32(base + S_REQID, reqId, true);
  view.setInt32(base + S_ISLAST, isLast, true);
  view.setInt32(base + S_ERRID, errId, true);
  view.setInt32(base + S_SID, sid, true);
  view.setInt32(base + S_PLEN, payloadLen, true);
  u8[base + S_ERRMSG] = 0;
}

const dispatch = (slotIdx) => md["dispatch"](slotIdx * info.slotSize);
// Use the real request() but with a fake successful send (returns 0). This is
// exactly what login()/logout() do, minus the native call that fails offline.
const fakeRequest = (single) => md["request"](() => 0, single);

async function settle(p, ms) {
  return Promise.race([
    p.then(() => ({ state: "resolved" })).catch((e) => ({ state: "rejected", e: String((e && e.message) || e) })),
    new Promise((r) => setTimeout(() => r({ state: "pending" }), ms)),
  ]);
}

async function main() {
  console.log("zeroIdFallbackEvent =", md["zeroIdFallbackEvent"], "| login evt =", MD_BASE + 4, "| logout evt =", MD_BASE + 5);

  // Case 1: login, id-0 response on the login event -> should RESOLVE via fallback.
  const loginP = fakeRequest(true);
  writeSlot(0, { evt: MD_BASE + 4, reqId: 0, isLast: 1, errId: 0, sid: SID_RspUserLogin, payloadLen: 4 });
  dispatch(0);
  console.log("login  (id0, evt login ):", (await settle(loginP, 150)).state);

  // Case 2: logout, id-0 response on the logout event -> bug if it stays pending.
  const logoutP = fakeRequest(true);
  writeSlot(1, { evt: MD_BASE + 5, reqId: 0, isLast: 1, errId: 0, sid: SID_UserLogout, payloadLen: 4 });
  dispatch(1);
  const r2 = await settle(logoutP, 300);
  console.log("logout (id0, evt logout):", r2.state, r2.state === "pending" ? "<-- BUG: never correlates; hangs until 30s timeout" : "");

  // Case 3 (control): if the server DID echo the id (like the trader), logout resolves.
  const logoutP3 = fakeRequest(true);
  // its id is the current reqSeq; read it via a fresh request and reuse the number
  const idNow = md["reqSeq"]; // last assigned id == logoutP3's id
  writeSlot(2, { evt: MD_BASE + 5, reqId: idNow, isLast: 1, errId: 0, sid: SID_UserLogout, payloadLen: 4 });
  dispatch(2);
  console.log("logout (echoed id)      :", (await settle(logoutP3, 150)).state, "(control: echoed id resolves)");

  process.exit(0);
}
main();
