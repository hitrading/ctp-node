/*
 * naming.mjs — identifier transforms (CTP → JS/TS conventions)
 *
 * Field/property names → camelCase with acronym handling
 *   InstrumentID → instrumentId, IPAddress → ipAddress, BidPrice1 → bidPrice1
 * Type/interface/enum names → strip CThostFtdc/TThostFtdc prefix + Field/Type
 *   suffix; CTP already uses PascalCase so we keep it as-is (recognizable).
 */

// Split an identifier into words, treating acronym runs and digit runs sensibly.
//   "InstrumentID"     -> ["Instrument", "ID"]
//   "IPAddress"        -> ["IP", "Address"]
//   "BidPrice1"        -> ["Bid", "Price", "1"]
//   "CFMMCTradingAcc"  -> ["CFMMC", "Trading", "Acc"]
export function splitWords(name) {
  const re = /[A-Z]+(?=[A-Z][a-z])|[A-Z][a-z]+|[A-Z]+|[a-z]+|[0-9]+/g;
  return name.match(re) || [];
}

// camelCase: first word fully lower-cased, subsequent words Capitalized.
// Acronyms become words (ID -> Id), so InstrumentID -> instrumentId.
export function camelCase(name) {
  const words = splitWords(name);
  if (words.length === 0) return name;
  return words
    .map((w, i) => {
      const lower = w.toLowerCase();
      return i === 0 ? lower : lower.charAt(0).toUpperCase() + lower.slice(1);
    })
    .join("");
}

// Struct C-name (CThostFtdcXxxField) -> TS type name (Xxx), casing preserved.
export function structTypeName(cName) {
  return cName.replace(/^CThostFtdc/, "").replace(/Field$/, "");
}

// Typedef name (TThostFtdcXxxType) -> enum/type name (Xxx), casing preserved.
export function typedefTypeName(tName) {
  return tName.replace(/^TThostFtdc/, "").replace(/Type$/, "");
}
