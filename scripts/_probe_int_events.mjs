// TEMP probe: verify sid=-2 int-payload events (front-disconnected reason,
// heart-beat-warning lapse) decode to the int32 value and emit correctly via
// the REAL dispatch path. Also verify front-disconnected triggers
// rejectAllPending against an in-flight request.
import { MarketData } from "../dist/index.js";
import { mkdirSync } from "node:fs";

mkdirSync("flow-test", { recursive: true });
const md = new MarketData("./flow-test/", "tcp://127.0.0.1:1");

const S_EVENT = 0, S_REQID = 4, S_ISLAST = 8, S_ERRID = 12, S_SID = 16, S_PLEN = 20, S_ERRMSG = 24;
const MD_BASE = 0x1000;
const info = md.native._info();
const ab = md.native._buffer();
const view = new DataView(ab);
const u8 = new Uint8Array(ab);

function writeIntSlot(slotIdx, evt, intValue, plen = 4) {
  const base = slotIdx * info.slotSize;
  view.setInt32(base + S_EVENT, evt, true);
  view.setInt32(base + S_REQID, 0, true);
  view.setInt32(base + S_ISLAST, -1, true); // streaming, not a response
  view.setInt32(base + S_ERRID, 0, true);
  view.setInt32(base + S_SID, -2, true);     // raw int32 payload sentinel
  view.setInt32(base + S_PLEN, plen, true);
  u8[base + S_ERRMSG] = 0;
  view.setInt32(base + info.headerSize, intValue | 0, true);
}
const dispatch = (slotIdx) => md["dispatch"](slotIdx * info.slotSize);

const captured = {};
md.on("front-disconnected", (reason) => { captured.disc = reason; });
md.on("heart-beat-warning", (lapse) => { captured.hb = lapse; });

// in-flight request to verify rejectAllPending on disconnect
let rejErr = null;
const pendingReq = md["request"](() => 0, true).catch((e) => { rejErr = String(e.message); });

// heartbeat warning, lapse = 7
writeIntSlot(0, MD_BASE + 3, 7);
dispatch(0);

// front-disconnected, reason = 0x1001 (a real CTP reason code: heartbeat timeout)
writeIntSlot(1, MD_BASE + 2, 0x1001);
dispatch(1);

// Edge: a disconnect with reason = a large negative (e.g. 4097 -> fine; test -1)
writeIntSlot(2, MD_BASE + 2, -1);
dispatch(2);

setTimeout(() => {
  console.log("heart-beat-warning lapse:", captured.hb, captured.hb === 7 ? "OK" : "FAIL");
  console.log("front-disconnected reason:", captured.disc, "(last write was -1)", captured.disc === -1 ? "OK" : "FAIL");
  console.log("in-flight request after disconnect:", rejErr ? `rejected: "${rejErr}"` : "STILL PENDING (unexpected)");
  md.close();
  process.exit(0);
}, 100);
