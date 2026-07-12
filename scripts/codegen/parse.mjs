/*
 * parse.mjs — minimal, regex-based parsers for the two CTP headers.
 *
 * These headers are extremely regular (no macros affecting layout, one field
 * per line with a /// comment above), so a line scanner is sufficient and far
 * simpler than pulling in a full C++ parser.
 */

import { readFile } from "node:fs/promises";
import { typedefTypeName } from "./naming.mjs";

// Official Windows CTP headers are GB18030/GBK. Decoding them as UTF-8 leaves
// tens of thousands of U+FFFD characters in generated TypeScript JSDoc.
const readHeader = async (path) =>
  new TextDecoder("gb18030").decode(await readFile(path));

// Value is usually a single char ('0'), but a few CTP enum families use
// multi-char codes (e.g. '102001'); capture the whole quoted value so none
// are silently dropped.
const RE_DEFINE = /^#define\s+THOST_FTDC_([A-Za-z0-9]+)_(\w+)\s+'([^']+)'/;
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
  const text = await readHeader(path);
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
      if (types.has(mArr[1]) || aliases.some(([alias]) => alias === mArr[1]))
        throw new Error(`duplicate CTP typedef declaration: ${mArr[1]}`);
      types.set(mArr[1], { kind: "string", size: Number(mArr[2]) });
      pending = [];
      continue;
    }

    const mScalar = RE_TD_SCALAR.exec(line);
    if (mScalar) {
      const [, base, name] = mScalar;
      if (types.has(name) || aliases.some(([alias]) => alias === name))
        throw new Error(`duplicate CTP typedef declaration: ${name}`);
      if (base === "char") {
        if (pending.length) {
          const enumName = typedefTypeName(name);
          types.set(name, { kind: "enum", size: 1, enumName, enumCtpType: name });
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
      if (types.has(mAlias[2]) || aliases.some(([alias]) => alias === mAlias[2]))
        throw new Error(`duplicate CTP typedef declaration: ${mAlias[2]}`);
      aliases.push([mAlias[2], mAlias[1]]); // newType -> existingType
      pending = [];
      continue;
    }

    if (/^typedef\b/.test(line) && /TThostFtdc\w+Type/.test(line)) {
      throw new Error(`unparsed CTP typedef declaration: ${line.trim()}`);
    }

    if (!RE_COMMENT.test(line) && !RE_BLANK.test(line)) pending = [];
  }

  // Resolve typedef-of-typedef aliases to a fixed point; header order must not
  // determine whether a chain is understood.
  let unresolved = aliases;
  for (;;) {
    const next = [];
    let progress = false;
    for (const [alias, target] of unresolved) {
      if (types.has(target)) {
        types.set(alias, { ...types.get(target) });
        progress = true;
      } else {
        next.push([alias, target]);
      }
    }
    unresolved = next;
    if (!progress || unresolved.length === 0) break;
  }
  if (unresolved.length)
    throw new Error(`unresolved CTP typedef aliases: ${unresolved.map(([a, t]) => `${a}->${t}`).join(", ")}`);

  return { types, enums };
}

/**
 * Parse the Struct header into:
 *   Array<{cName, typeName, fields: [{cName, ctpType, comment}]}>
 * "reserveN" fields (obsolete padding placeholders) are dropped.
 */
export async function parseStructs(path) {
  const text = await readHeader(path);
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
      if (cur)
        throw new Error(`nested/unclosed CTP struct declaration: ${cur.cName}`);
      if (structs.some((item) => item.cName === mS[1]))
        throw new Error(`duplicate CTP struct declaration: ${mS[1]}`);
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
      continue;
    }
    if (/TThostFtdc\w+Type/.test(line) && /;/.test(line))
      throw new Error(`unparsed field declaration in ${cur.cName}: ${line.trim()}`);
  }

  if (cur)
    throw new Error(`unterminated CTP struct declaration: ${cur.cName}`);

  return structs;
}
