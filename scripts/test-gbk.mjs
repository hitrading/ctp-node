// Outbound GB18030 encoding round-trip (no network): native __gbkEncode ->
// TextDecoder('gb18030'), and an encodeStruct(gbk) -> decode round-trip on a
// real struct string field.
import { createRequire } from "node:module";
import { existsSync } from "node:fs";
import { parseLayouts, buildDecoder, encodeStruct } from "../dist/codec.js";
import { STRUCTS } from "../dist/generated/structs.gen.js";

const require = createRequire(import.meta.url);
const native = require(
  ["../build/Release/ctp.node", "../build/Debug/ctp.node"].find((p) =>
    existsSync(new URL(p, import.meta.url))
  )
);

let pass = 0;
let fail = 0;
const check = (c, m) => (c ? (pass++, console.log("  PASS:", m)) : (fail++, console.error("  FAIL:", m)));

const gbkEnc = (str) => {
  for (let i = 0; i < str.length; i++)
    if (str.charCodeAt(i) > 0x7f) return new Uint8Array(native.__gbkEncode(str));
  const a = new Uint8Array(str.length);
  for (let i = 0; i < str.length; i++) a[i] = str.charCodeAt(i);
  return a;
};

// 1) direct encode -> decode
const s = "测试中文ABC123";
const back = new TextDecoder("gb18030").decode(new Uint8Array(native.__gbkEncode(s)));
check(back === s, `direct gbk round-trip: "${back}"`);

// 2) encodeStruct(gbk) -> decode on a real struct field (size >= 16)
const layouts = parseLayouts(native.__layoutData());
let id = -1;
let field = null;
for (let i = 0; i < STRUCTS.length; i++) {
  const fi = STRUCTS[i].fields.findIndex((f) => f.kind === 0);
  if (fi >= 0 && layouts[i].fields[fi] && layouts[i].fields[fi].size >= 16) {
    id = i;
    field = STRUCTS[i].fields[fi].js;
    break;
  }
}
const cn = "测试客户名称";
const bytes = encodeStruct(layouts[id], STRUCTS[id].fields, { [field]: cn }, gbkEnc);
const obj = buildDecoder(id, layouts[id])(
  new DataView(bytes.buffer),
  new Uint8Array(bytes.buffer),
  0
);
check(obj[field] === cn, `struct field round-trip ${STRUCTS[id].name}.${field} = "${obj[field]}"`);

console.log(`GBK TEST: ${pass} pass, ${fail} fail`);
process.exitCode = fail ? 1 : 0;
