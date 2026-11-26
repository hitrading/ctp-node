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

/** TThostFtdcInvestorRangeType: THOST_FTDC_IR_All '1', THOST_FTDC_IR_Group
 * '2', THOST_FTDC_IR_Single '3'. */
export const CTP_INVESTOR_RANGE_GROUP = '2';
/** A narrower range overrides a wider one: single > group > all. */
const RANGE_PRIORITY: Readonly<Record<string, number>> = { '3': 3, '2': 2, '1': 1 };

interface CtpMarginSchedule {
  longMarginRatioByMoney: number;
  longMarginRatioByVolume: number;
  shortMarginRatioByMoney: number;
  shortMarginRatioByVolume: number;
}
/** The attribution fields of a CThostFtdcInstrumentMarginRateField row. */
export interface CtpInvestorMarginRow extends CtpMarginSchedule {
  instrumentId: string;
  exchangeId: string;
  investorRange: string;
  brokerId: string;
  investorId: string;
  investUnitId: string;
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
export interface CtpMarginAccount {
  brokerId: string;
  investorId: string;
  /** Required whenever the rows contain an investor-group (range 2) row. */
  investorGroupId?: string;
}
export interface CtpMarginRowSelection<T> {
  /** The one applicable row per instrument/product id. */
  rates: Map<string, T>;
  /** One diagnostic per row that does not belong to this account. */
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

/** Broker, investment unit and exchange: the attribution that needs no
 * investor-group lookup. */
function investorRowScopeExclusion(row: CtpInvestorMarginRow, brokerId: string, exchangeId: string): string | undefined {
  if (row.brokerId !== brokerId) return `broker ${row.brokerId}`;
  if (row.investUnitId !== '') return `investment unit ${row.investUnitId}`;
  return exchangeExclusion(row, exchangeId);
}

/** Whether judging these rows needs the account's investor group: only an
 * investor-group row that is otherwise attributable to this account does. */
export function ctpInvestorGroupNeeded(rows: readonly CtpInvestorMarginRow[], brokerId: string, exchangeId: string): boolean {
  return rows.some(row => row.investorRange === CTP_INVESTOR_RANGE_GROUP &&
    !investorRowScopeExclusion(row, brokerId, exchangeId));
}

function investorRowExclusion(row: CtpInvestorMarginRow, account: CtpMarginAccount, exchangeId: string): string | undefined {
  const scope = investorRowScopeExclusion(row, account.brokerId, exchangeId);
  if (scope) return scope;
  switch (row.investorRange) {
    // An all-investor row may carry a wildcard InvestorID; never compare it.
    case '1': return undefined;
    case '2':
      if (!account.investorGroupId)
        throw new Error('CTP investor group is required to judge an investor-group margin row');
      return row.investorId === account.investorGroupId ? undefined : `investor group ${row.investorId}`;
    case '3': return row.investorId === account.investorId ? undefined : `investor ${row.investorId}`;
    default: return `investor range ${row.investorRange}`;
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

/** Keep the investor margin rows that belong to this account and to the
 * contract's exchange and pick, per instrument/product id, the narrowest
 * applicable investor range. */
export function selectCtpInvestorMarginRates<T extends CtpInvestorMarginRow>(
  rows: readonly T[], account: CtpMarginAccount, exchangeId: string
): CtpMarginRowSelection<T> {
  const excluded: string[] = [];
  const accepted = rows.filter(row => {
    const reason = investorRowExclusion(row, account, exchangeId);
    if (reason) excluded.push(`investor ${row.instrumentId}: ${reason}`);
    return !reason;
  });
  const rates = new Map<string, T>();
  for (const [key, candidates] of groupRows(accepted)) {
    const priority = Math.max(...candidates.map(row => RANGE_PRIORITY[row.investorRange]));
    const range = candidates.filter(row => RANGE_PRIORITY[row.investorRange] === priority);
    rates.set(key, uniqueRow(range, INVESTOR_FIELDS,
      `investor margin rate ${key} (range ${range[0].investorRange})`));
  }
  return { rates, excluded };
}

/** Keep the exchange base rows of this account's broker and the contract's
 * exchange, one per instrument. */
export function selectCtpExchangeMarginRates<T extends CtpExchangeMarginRow>(
  rows: readonly T[], brokerId: string, exchangeId: string
): CtpMarginRowSelection<T> {
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
  const detail = excluded.length ? `; excluded rows of other accounts: ${shown}${more}` : '';
  return new Error(`CTP ${kind} rate is missing: ${key}${detail}`);
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
  rows: ReadonlyMap<string, T>, instrumentId: string, productId: string,
  excluded: readonly string[] = []
): T {
  if (rows.has(instrumentId)) return rows.get(instrumentId)!;
  if (rows.has(productId)) return rows.get(productId)!;
  throw ctpMarginRateMissing('product/contract', `${productId}/${instrumentId}`, excluded);
}
