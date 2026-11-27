/** CTP product schedules are inherited only through the current instrument
 * catalog. An explicit contract row (even an invalid one) always wins. */
export interface CtpProductInstrument { instrumentId: string; productId: string; exchangeId?: string; }
export interface CtpResolvedMarginRate {
  instrumentId: string;
  hedgeFlag: '1' | '2' | '3' | '5' | '6' | '7';
  longMarginRatioByMoney: number;
  longMarginRatioByVolume: number;
  shortMarginRatioByMoney: number;
  shortMarginRatioByVolume: number;
}

/** TThostFtdcInvestorRangeType: THOST_FTDC_IR_All '1', THOST_FTDC_IR_Group
 * '2', THOST_FTDC_IR_Single '3'. */
export const CTP_INVESTOR_RANGE_GROUP = '2';
/** TThostFtdcBizTypeType: THOST_FTDC_BZTP_Future '1' (THOST_FTDC_BZTP_Stock is '2'). */
const CTP_BIZ_TYPE_FUTURE = '1';
/** A narrower range overrides a wider one: single > group > all. */
const RANGE_PRIORITY: Readonly<Record<string, number>> = { '3': 3, '2': 2, '1': 1 };

interface CtpMarginSchedule {
  longMarginRatioByMoney: number;
  longMarginRatioByVolume: number;
  shortMarginRatioByMoney: number;
  shortMarginRatioByVolume: number;
}
/** The attribution fields every investor rate row carries: margin
 * (CThostFtdcInstrumentMarginRateField) and commission
 * (CThostFtdcInstrumentCommissionRateField, which also names its BizType). */
export interface CtpInvestorRateRow {
  instrumentId: string;
  exchangeId: string;
  investorRange: string;
  brokerId: string;
  investorId: string;
  investUnitId: string;
  /** Futures commission replies may leave BizType empty. Margin rows have no BizType. */
  bizType?: string | null;
}
/** The attribution fields of a CThostFtdcInstrumentMarginRateField row. */
export interface CtpInvestorMarginRow extends CtpInvestorRateRow, CtpMarginSchedule {
  isRelative: number;
}
/** The attribution fields of a CThostFtdcExchangeMarginRateField row. */
export interface CtpExchangeMarginRow extends CtpMarginSchedule {
  instrumentId: string;
  exchangeId: string;
  brokerId: string;
}
/** The confirmed account a rate row must belong to. The Trader sends every
 * order without an investment unit, so a row scoped to any investment unit
 * never applies to it. */
export interface CtpRateAccount {
  brokerId: string;
  investorId: string;
  /** Required for applicable group rows without a narrower personal row of the same key. */
  investorGroupId?: string;
}
/** The requested contracts of one catalog product on its confirmed exchange. */
export interface CtpProductRateRequest {
  exchangeId: string;
  productId: string;
  instrumentIds: readonly string[];
}
export interface CtpRateRowSelection<T> {
  /** The one applicable row per instrument/product id. */
  rates: Map<string, T>;
  /** Reasons rate rows were excluded from this request. */
  excluded: string[];
}

const SCHEDULE_FIELDS = ['longMarginRatioByMoney', 'longMarginRatioByVolume',
  'shortMarginRatioByMoney', 'shortMarginRatioByVolume'] as const;
const INVESTOR_FIELDS = [...SCHEDULE_FIELDS, 'isRelative'] as const;

/** A row naming another exchange belongs to another contract; an empty
 * ExchangeID carries no exchange attribution and is judged by its key. */
function exchangeExclusion(row: { exchangeId: string }, exchangeId: string): string | undefined {
  return row.exchangeId !== '' && row.exchangeId !== exchangeId ? `exchange ${row.exchangeId}` : undefined;
}

export type InvestorRateKind = 'margin' | 'commission';

function investorRowScopeExclusion(row: CtpInvestorRateRow, brokerId: string, exchangeId: string, kind: InvestorRateKind): string | undefined {
  if (kind === 'commission') {
    const business = row.bizType;
    // Quote the code: a blank or other invisible byte must stay readable in the diagnostic.
    if (business != null && business !== '' && business !== CTP_BIZ_TYPE_FUTURE)
      return `business type ${JSON.stringify(business)}`;
  }
  if (row.brokerId !== brokerId) return `broker ${row.brokerId}`;
  if (row.investUnitId !== '') return `investment unit ${row.investUnitId}`;
  return exchangeExclusion(row, exchangeId);
}

/** Exclude foreign scopes before deciding whether a personal row supersedes a group row.
 * Keep every applicable personal row so conflicting values are still rejected. */
function investorCandidates<T extends CtpInvestorRateRow>(
  rows: readonly T[], account: CtpRateAccount, request: CtpProductRateRequest, kind: InvestorRateKind, excluded?: string[]
): T[] {
  if (kind !== 'margin' && kind !== 'commission') throw new TypeError('expected margin or commission rate kind');
  if (!Array.isArray(request.instrumentIds)) throw new TypeError('expected instrumentIds in the product rate request');
  const requested = new Set(request.instrumentIds);
  const needsProduct = new Set(requested);
  const personalKeys = new Set<string>();
  const scoped = rows.filter(row => {
    const key = row.instrumentId;
    if (key !== request.productId && !requested.has(key)) return false;
    const reason = investorRowScopeExclusion(row, account.brokerId, request.exchangeId, kind);
    if (reason) {
      excluded?.push(`investor ${row.instrumentId}: ${reason}`);
      return false;
    }
    // A group-only contract may still need the product until its group is confirmed.
    if ((row.investorRange !== CTP_INVESTOR_RANGE_GROUP || account.investorGroupId) && !investorRowExclusion(row, account)) {
      if (key !== request.productId) needsProduct.delete(key);
      if (row.investorRange === '3') personalKeys.add(key);
    }
    return true;
  });
  return scoped.filter(row => (row.instrumentId !== request.productId || needsProduct.size > 0) &&
    (row.investorRange !== CTP_INVESTOR_RANGE_GROUP || !personalKeys.has(row.instrumentId)));
}

/** A group lookup is needed only for a requested contract or a product it may inherit. */
export function ctpInvestorGroupNeeded(
  rows: readonly CtpInvestorRateRow[], account: CtpRateAccount, request: CtpProductRateRequest, kind: InvestorRateKind
): boolean {
  const candidates = investorCandidates(rows, account, request, kind);
  return !account.investorGroupId && candidates.some(row => row.investorRange === CTP_INVESTOR_RANGE_GROUP);
}

function investorRowExclusion(row: CtpInvestorRateRow, account: CtpRateAccount): string | undefined {
  switch (row.investorRange) {
    // An all-investor row may carry a wildcard InvestorID; never compare it.
    case '1': return undefined;
    case '2':
      if (!account.investorGroupId)
        throw new Error('CTP investor group is required to judge an investor-group rate row');
      return row.investorId === account.investorGroupId ? undefined : `investor group ${row.investorId}`;
    case '3': return row.investorId === account.investorId ? undefined : `investor ${row.investorId}`;
    default: return `investor range ${JSON.stringify(row.investorRange)}`;
  }
}

/** Several rows of one key are acceptable only when they agree exactly. */
function uniqueRow<T>(rows: readonly T[], fields: readonly (keyof T)[], label: string): T {
  const [first] = rows;
  if (rows.some(row => fields.some(field => !Object.is(row[field], first[field]))))
    throw new Error(`CTP ${label} has conflicting rows`);
  return first;
}

function groupRows<T extends { instrumentId: string }>(rows: readonly T[]): Map<string, T[]> {
  const groups = new Map<string, T[]>();
  for (const row of rows) {
    const members = groups.get(row.instrumentId);
    if (members) members.push(row);
    else groups.set(row.instrumentId, [row]);
  }
  return groups;
}

/** Keep the investor rate rows (margin or commission) that belong to this
 * account, to futures business and to the contract's exchange, and pick, per
 * instrument/product id, the narrowest applicable investor range; several rows
 * of that range must agree on `fields`. The one attribution rule margin and
 * commission share. */
export function selectCtpInvestorRates<T extends CtpInvestorRateRow>(
  rows: readonly T[], account: CtpRateAccount, request: CtpProductRateRequest, fields: readonly (keyof T)[], kind: InvestorRateKind
): CtpRateRowSelection<T> {
  const excluded: string[] = [];
  const accepted = investorCandidates(rows, account, request, kind, excluded).filter(row => {
    const reason = investorRowExclusion(row, account);
    if (reason) excluded.push(`investor ${row.instrumentId}: ${reason}`);
    return !reason;
  });
  const rates = new Map<string, T>();
  const label = kind === 'margin' ? 'investor margin rate' : 'commission rate';
  for (const [key, candidates] of groupRows(accepted)) {
    const priority = Math.max(...candidates.map(row => RANGE_PRIORITY[row.investorRange]));
    const range = candidates.filter(row => RANGE_PRIORITY[row.investorRange] === priority);
    rates.set(key, uniqueRow(range, fields, `${label} ${key} (range ${range[0].investorRange})`));
  }
  return { rates, excluded };
}

/** {@link selectCtpInvestorRates} for investor margin rows. */
export function selectCtpInvestorMarginRates<T extends CtpInvestorMarginRow>(
  rows: readonly T[], account: CtpRateAccount, request: CtpProductRateRequest
): CtpRateRowSelection<T> {
  return selectCtpInvestorRates(rows, account, request, INVESTOR_FIELDS, 'margin');
}

/** Keep the exchange base rows of this account's broker and the contract's
 * exchange, one per instrument. */
export function selectCtpExchangeMarginRates<T extends CtpExchangeMarginRow>(
  rows: readonly T[], brokerId: string, exchangeId: string
): CtpRateRowSelection<T> {
  const excluded: string[] = [];
  const accepted = rows.filter(row => {
    const reason = row.brokerId !== brokerId ? `broker ${row.brokerId}` : exchangeExclusion(row, exchangeId);
    if (reason) excluded.push(`exchange ${row.instrumentId}: ${reason}`);
    return !reason;
  });
  const rates = new Map<string, T>();
  for (const [key, candidates] of groupRows(accepted))
    rates.set(key, uniqueRow(candidates, SCHEDULE_FIELDS, `exchange margin rate ${key}`));
  return { rates, excluded };
}

/** At most this many excluded rows are named in one error; a full catalog can
 *  exclude thousands and the message must stay loggable. */
const EXCLUDED_ROWS_SHOWN = 10;

/** A missing rate, naming the rows excluded because they belong elsewhere. */
export function ctpMarginRateMissing(kind: string, key: string, excluded: readonly string[]): Error {
  const shown = excluded.slice(0, EXCLUDED_ROWS_SHOWN).join('; ');
  const more = excluded.length > EXCLUDED_ROWS_SHOWN ? `; and ${excluded.length - EXCLUDED_ROWS_SHOWN} more` : '';
  const detail = excluded.length ? `; excluded rate rows: ${shown}${more}` : '';
  return new Error(`CTP ${kind} rate is missing: ${key}${detail}`);
}

/** The one exchange a product's rate rows are attributed to: every catalog row
 * of the product (not only the requested months) must name it, and a row
 * without a contract code names no contract and takes no part. */
export function ctpProductExchange(catalog: readonly CtpProductInstrument[], productId: string): string {
  const exchanges = new Set(catalog
    .filter(row => typeof row.instrumentId === 'string' && row.instrumentId.trim() !== '' && row.productId === productId)
    .map(row => row.exchangeId));
  const [exchangeId] = exchanges;
  if (exchanges.size !== 1 || typeof exchangeId !== 'string' || !exchangeId)
    throw new Error(`CTP product ${productId} has no single exchange in the instrument catalog`);
  return exchangeId;
}

export function groupCtpInstrumentsByProduct(
  instrumentIds: readonly string[], catalog: readonly CtpProductInstrument[]
): Map<string, string[]> {
  // A requested contract must have one catalog identity: rows of it that
  // disagree on product or exchange make it ambiguous, never last-row-wins.
  const requested = new Set(instrumentIds);
  const products = new Map<string, CtpProductInstrument>();
  for (const row of catalog) {
    if (!requested.has(row.instrumentId)) continue;
    const seen = products.get(row.instrumentId);
    if (seen && (seen.productId !== row.productId || seen.exchangeId !== row.exchangeId))
      throw new Error(`CTP instrument identity conflicts in the catalog: ${row.instrumentId}`);
    products.set(row.instrumentId, row);
  }
  const groups = new Map<string, string[]>();
  for (const instrumentId of requested) {
    const productId = products.get(instrumentId)?.productId;
    if (typeof productId !== 'string' || !productId.trim() || productId.includes('\0'))
      throw new Error(`CTP product identity is missing: ${instrumentId}`);
    const members = groups.get(productId);
    if (members) members.push(instrumentId);
    else groups.set(productId, [instrumentId]);
  }
  return groups;
}

export function selectCtpProductRate<T>(
  rows: ReadonlyMap<string, T>, instrumentId: string, productId: string,
  excluded: readonly string[] = []
): T {
  if (rows.has(instrumentId)) return rows.get(instrumentId)!;
  if (rows.has(productId)) return rows.get(productId)!;
  throw ctpMarginRateMissing('product/contract', `${productId}/${instrumentId}`, excluded);
}
