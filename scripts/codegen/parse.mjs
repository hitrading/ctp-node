/*
 * parse.mjs — minimal, regex-based parsers for the two CTP headers.
 *
 * These headers are extremely regular (no macros affecting layout, one field
 * per line with a /// comment above), so a line scanner is sufficient and far
 * simpler than pulling in a full C++ parser.
 */

import { readFile } from "node:fs/promises";
import { typedefTypeName } from "./naming.mjs";

const RE_DEFINE = /^#define\s+THOST_FTDC_([A-Za-z0-9]+)_(\w+)\s+'(.)'/;
const RE_TD_ARRAY = /^typedef\s+char\s+(TThostFtdc\w+Type)\s*\[\s*(\d+)\s*\]/;
const RE_TD_SCALAR = /^typedef\s+(char|int|short|double)\s+(TThostFtdc\w+Type)\s*;/;
const RE_TD_ALIAS = /^typedef\s+(TThostFtdc\w+Type)\s+(TThostFtdc\w+Type)\s*;/;
const RE_COMMENT = /^\s*\/\//;
const RE_BLANK = /^\s*$/;

const SCALAR_KIND = {
  int: { kind: "int32", size: 4 },
  short: { kind: "int16", size: 2 },
  double: { kind: "double", size: 8 },
};

/**
 * Parse the DataType header into:
 *   types: Map<ctpTypeName, {kind, size, enumName?}>
 *   enums: Array<{name, ctpType, members: [{name, value}]}>
 */
export async function parseDataTypes(path) {
  const text = await readFile(path, "utf8");
  const lines = text.split(/\r?\n/);

  const types = new Map();
  const enums = [];
  const aliases = [];
  let pending = []; // buffered #define members awaiting their typedef

  for (const line of lines) {
    const mDef = RE_DEFINE.exec(line);
    if (mDef) {
      pending.push({ name: mDef[2], value: mDef[3] });
      continue;
    }

    const mArr = RE_TD_ARRAY.exec(line);
    if (mArr) {
      types.set(mArr[1], { kind: "string", size: Number(mArr[2]) });
      pending = [];
      continue;
    }

    const mScalar = RE_TD_SCALAR.exec(line);
    if (mScalar) {
      const [, base, name] = mScalar;
      if (base === "char") {
        if (pending.length) {
          const enumName = typedefTypeName(name);
          types.set(name, { kind: "enum", size: 1, enumName });
          enums.push({ name: enumName, ctpType: name, members: pending });
        } else {
          types.set(name, { kind: "char", size: 1 });
        }
      } else {
        types.set(name, { ...SCALAR_KIND[base] });
      }
      pending = [];
      continue;
    }

    const mAlias = RE_TD_ALIAS.exec(line);
    if (mAlias) {
      aliases.push([mAlias[2], mAlias[1]]); // newType -> existingType
      pending = [];
      continue;
    }

    if (!RE_COMMENT.test(line) && !RE_BLANK.test(line)) pending = [];
  }

  // Resolve typedef-of-typedef aliases (rare, but be correct).
  for (const [alias, target] of aliases) {
    if (types.has(target)) types.set(alias, { ...types.get(target) });
  }

  return { types, enums };
}

/**
 * Parse the Struct header into:
 *   Array<{cName, typeName, fields: [{cName, ctpType, comment}]}>
 * "reserveN" fields (deprecated padding placeholders) are dropped.
 */
export async function parseStructs(path) {
  const text = await readFile(path, "utf8");
  const lines = text.split(/\r?\n/);

  const RE_STRUCT = /^struct\s+(CThostFtdc\w+Field)/;
  const RE_FIELD = /^\s*(TThostFtdc\w+Type)\s+(\w+)\s*;/;
  const RE_FCOMMENT = /^\s*\/\/\/(.*)$/;
  const RE_END = /^\s*}\s*;/;

  const structs = [];
  let cur = null;
  let comment = "";

  for (const line of lines) {
    const mS = RE_STRUCT.exec(line);
    if (mS) {
      cur = { cName: mS[1], fields: [] };
      comment = "";
      continue;
    }
    if (!cur) continue;

    if (RE_END.test(line)) {
      structs.push(cur);
      cur = null;
      continue;
    }
    const mC = RE_FCOMMENT.exec(line);
    if (mC) {
      comment = mC[1].trim();
      continue;
    }
    const mF = RE_FIELD.exec(line);
    if (mF) {
      const fieldName = mF[2];
      if (!/^reserve\d+$/.test(fieldName)) {
        cur.fields.push({ cName: fieldName, ctpType: mF[1], comment });
      }
      comment = "";
    }
  }

  return structs;
}
