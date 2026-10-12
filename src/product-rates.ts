/** CTP product schedules are inherited only through the current instrument
 * catalog. An explicit contract row (even an invalid one) always wins. */
export interface CtpProductInstrument { instrumentId: string; productId: string; }
export interface CtpResolvedMarginRate {
  instrumentId: string;
  hedgeFlag: '1' | '2' | '3' | '5' | '6' | '7';
  longMarginRatioByMoney: number;
  longMarginRatioByVolume: number;
  shortMarginRatioByMoney: number;
  shortMarginRatioByVolume: number;
}

export function groupCtpInstrumentsByProduct(
  instrumentIds: readonly string[], catalog: readonly CtpProductInstrument[]
): Map<string, string[]> {
  const products = new Map(catalog.map(row => [row.instrumentId, row.productId]));
  const groups = new Map<string, string[]>();
  for (const instrumentId of new Set(instrumentIds)) {
    const productId = products.get(instrumentId);
    if (typeof productId !== 'string' || !productId.trim() || productId.includes('\0'))
      throw new Error(`CTP product identity is missing: ${instrumentId}`);
    const members = groups.get(productId);
    if (members) members.push(instrumentId);
    else groups.set(productId, [instrumentId]);
  }
  return groups;
}

export function selectCtpProductRate<T>(
  rows: ReadonlyMap<string, T>, instrumentId: string, productId: string
): T {
  if (rows.has(instrumentId)) return rows.get(instrumentId)!;
  if (rows.has(productId)) return rows.get(productId)!;
  throw new Error(`CTP product/contract rate is missing: ${productId}/${instrumentId}`);
}
