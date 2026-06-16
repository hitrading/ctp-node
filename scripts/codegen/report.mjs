/*
 * report.mjs — validation report for the naming + parsing, BEFORE we commit to
 * generating all 466 structs. Run: node scripts/codegen/report.mjs
 */

import { parseDataTypes, parseStructs } from "./parse.mjs";
import { camelCase, structTypeName } from "./naming.mjs";

const DT = new URL("../../ctpapi/ThostFtdcUserApiDataType.h", import.meta.url);
const ST = new URL("../../ctpapi/ThostFtdcUserApiStruct.h", import.meta.url);

const { types, enums } = await parseDataTypes(DT);
const structs = await parseStructs(ST);

console.log(`parsed: ${types.size} types, ${enums.length} enums, ${structs.length} structs\n`);

// 1) Hot-path struct, fully resolved to JS shape.
const dmd = structs.find((s) => s.cName === "CThostFtdcDepthMarketDataField");
console.log(`interface ${structTypeName(dmd.cName)} {  // ${dmd.fields.length} fields`);
for (const f of dmd.fields) {
  const t = types.get(f.ctpType);
  const tsType =
    !t ? "??" :
    t.kind === "double" || t.kind === "int32" || t.kind === "int16" ? "number" :
    t.kind === "enum" ? t.enumName :
    "string";
  console.log(`  ${camelCase(f.cName).padEnd(20)}: ${String(tsType).padEnd(10)} // ${f.comment}`);
}
console.log("}\n");

// 2) A sample enum.
const dir = enums.find((e) => e.name === "Direction");
console.log(`enum ${dir.name} {`);
for (const m of dir.members) console.log(`  ${m.name} = "${m.value}",`);
console.log("}\n");

// 3) Acronym / tricky-name spot checks.
console.log("naming spot-checks:");
for (const n of [
  "InstrumentID", "IPAddress", "BidPrice1", "AskVolume5", "BrokerID", "UserID",
  "SHFETime", "CZCETime", "CFMMCTradingAccountKey", "UpdateMillisec",
  "MaxOrderRef", "OrderSysID", "ExchangeInstID", "CombHedgeFlag", "BizType",
]) {
  console.log(`  ${n.padEnd(24)} -> ${camelCase(n)}`);
}
console.log();

// 4) Collision check: do any two CTP fields in one struct collapse to the same camelCase?
let collisions = 0;
for (const s of structs) {
  const seen = new Map();
  for (const f of s.fields) {
    const c = camelCase(f.cName);
    if (seen.has(c) && seen.get(c) !== f.cName) {
      collisions++;
      console.log(`  COLLISION in ${s.cName}: ${seen.get(c)} & ${f.cName} -> ${c}`);
    }
    seen.set(c, f.cName);
  }
}
console.log(collisions === 0 ? "no camelCase collisions within any struct\n" : `${collisions} collisions!\n`);

// 5) Coverage: any field type we failed to resolve?
const unresolved = new Set();
for (const s of structs)
  for (const f of s.fields) if (!types.has(f.ctpType)) unresolved.add(f.ctpType);
console.log(
  unresolved.size === 0
    ? "all struct field types resolved"
    : `UNRESOLVED types (${unresolved.size}): ${[...unresolved].join(", ")}`
);
