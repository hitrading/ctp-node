import assert from 'node:assert/strict';
import { mkdirSync } from 'node:fs';
import simnowAo from '../test/fixtures/simnow-20261127-ao-commission.json' with { type: 'json' };
import { groupCtpInstrumentsByProduct, selectCtpProductRate, Trader } from '../dist/index.js';
import { ctpInvestorGroupNeeded, ctpMarginRateMissing, selectCtpInvestorMarginRates, selectCtpInvestorRates } from '../dist/product-rates.js';
const catalog = [{ instrumentId: 'rb2610', productId: 'rb' }, { instrumentId: 'rb2701', productId: 'rb' },
  { instrumentId: 'IF2610', productId: 'IF' }];
assert.deepEqual([...groupCtpInstrumentsByProduct(['rb2610', 'rb2701', 'IF2610', 'rb2610'], catalog)],
  [['rb', ['rb2610', 'rb2701']], ['IF', ['IF2610']]]);
for (const productId of [undefined, null, '', ' ', 'rb\0'])
  assert.throws(() => groupCtpInstrumentsByProduct(['rb2610'], [{ instrumentId: 'rb2610', productId }]));
assert.throws(() => groupCtpInstrumentsByProduct(['unknown'], catalog));
// A requested contract needs one catalog identity; unrequested conflicts do not matter.
assert.deepEqual([...groupCtpInstrumentsByProduct(['rb2610'], [...catalog, { instrumentId: 'rb2610', productId: 'rb' },
  { instrumentId: 'IF2610', productId: 'rb' }])], [['rb', ['rb2610']]]);
for (const conflicting of [{ instrumentId: 'rb2610', productId: 'hc' }, { instrumentId: 'rb2610', productId: 'rb', exchangeId: 'DCE' }])
  assert.throws(() => groupCtpInstrumentsByProduct(['rb2610'], [...catalog, conflicting]),
    { message: 'CTP instrument identity conflicts in the catalog: rb2610' });
const rates = new Map([['rb', .1], ['rb2701', .2], ['rb2710', NaN]]);
assert.equal(selectCtpProductRate(rates, 'rb2610', 'rb'), .1);
assert.equal(selectCtpProductRate(rates, 'rb2701', 'rb'), .2);
assert(Number.isNaN(selectCtpProductRate(rates, 'rb2710', 'rb')));
assert.throws(() => selectCtpProductRate(rates, 'IF2610', 'IF'),
  { message: 'CTP product/contract rate is missing: IF/IF2610' });

// Margin-rate attribution: Trader.syncMarginRates publishes only rows that
// belong to the confirmed account (broker 9999, investor u, no invest unit).
const tests = [];
const test = (name, fn) => tests.push([name, fn]);
const own = (instrumentId, ratio, extra = {}) => ({
  instrumentId, hedgeFlag: '1', investorRange: '3', brokerId: '9999', investorId: 'u',
  investUnitId: '', exchangeId: 'SHFE', isRelative: 0,
  longMarginRatioByMoney: ratio, longMarginRatioByVolume: 0,
  shortMarginRatioByMoney: ratio, shortMarginRatioByVolume: 0, ...extra,
});
const base = (instrumentId, ratio, extra = {}) => ({
  instrumentId, hedgeFlag: '1', brokerId: '9999', exchangeId: 'SHFE',
  longMarginRatioByMoney: ratio, longMarginRatioByVolume: 0,
  shortMarginRatioByMoney: ratio, shortMarginRatioByVolume: 0, ...extra,
});
mkdirSync('flow-product-rates', { recursive: true });
const td = new Trader('./flow-product-rates/', 'tcp://127.0.0.1:1');
td.queryRetryDelayMs = 0;
td.broker = '9999';
td.investor = 'u';
const shfeCatalog = [
  { instrumentId: 'rb2610', productId: 'rb', exchangeId: 'SHFE', volumeMultiple: 10 },
  { instrumentId: 'rb2701', productId: 'rb', exchangeId: 'SHFE', volumeMultiple: 10 },
  { instrumentId: 'hc2610', productId: 'hc', exchangeId: 'SHFE', volumeMultiple: 10 },
];
const rbRequest = { productId: 'rb', instrumentIds: ['rb2610'], exchangeId: 'SHFE' };
td.reqQryInstrument = async () => shfeCatalog;
let investorQueries = [];
let investorReply = [{ brokerId: '9999', investorId: 'u', investorGroupId: 'G1' }];
td.reqQryInvestor = async (request) => { investorQueries.push(request); return investorReply; };
const stub = (investorRows, exchangeRows = []) => {
  investorQueries = [];
  td.reqQryInstrumentMarginRate = async () => investorRows;
  td.reqQryExchangeMarginRate = async () => exchangeRows;
};
const longRate = async (investorRows, exchangeRows, instrumentId = 'rb2610') => {
  stub(investorRows, exchangeRows);
  assert.equal(await td.syncMarginRates([instrumentId], '1'), 1);
  return td.readMarginRates([instrumentId], '1')[0].longMarginRatioByMoney;
};
const failsWith = async (investorRows, exchangeRows, message, instrumentId = 'rb2610') => {
  stub(investorRows, exchangeRows);
  await assert.rejects(td.syncMarginRates([instrumentId], '1'), { message });
  assert.throws(() => td.readMarginRates([instrumentId], '1'), /synchronized margin rate is missing/);
};

test('audit: another broker single-investor row does not replace the own 12%', async () =>
  assert.equal(await longRate([own('rb2610', .12), own('rb2610', .04, { brokerId: '8888' })]), .12));
test('audit: another single investor row does not replace the own 12%', async () =>
  assert.equal(await longRate([own('rb2610', .12), own('rb2610', .04, { investorId: 'other' })]), .12));
test('audit: another investment unit row does not replace the own 12%', async () =>
  assert.equal(await longRate([own('rb2610', .12), own('rb2610', .04, { investUnitId: 'unit-x' })]), .12));
test('audit: relative 2% adds only the own broker exchange base 10%', async () => {
  const rate = await longRate([own('rb2610', .02, { isRelative: 1 })],
    [base('rb2610', .1), base('rb2610', .04, { brokerId: '8888' })]);
  assert(Math.abs(rate - .12) < 1e-12, `expected 0.12, got ${rate}`);
});
test('all-investor rows apply whatever wildcard InvestorID they carry', async () => {
  assert.equal(await longRate([own('rb2610', .09, { investorRange: '1', investorId: '00000000' })]), .09);
  assert.equal(await longRate([own('rb2610', .09, { investorRange: '1', investorId: '' })]), .09);
  assert.equal(investorQueries.length, 0, 'no investor-group query without a group row');
});
test('investor-group rows match the account InvestorGroupID only', async () => {
  investorReply = [{ brokerId: '9999', investorId: 'u', investorGroupId: 'G1' }];
  assert.equal(await longRate([own('rb2610', .03, { investorRange: '2', investorId: 'G2' }),
    own('rb2610', .15, { investorRange: '2', investorId: 'G1' })]), .15);
  assert.deepEqual(investorQueries, [{ brokerId: '9999', investorId: 'u' }]);
  await failsWith([own('rb2610', .03, { investorRange: '2', investorId: 'G2' })], [],
    'CTP product/contract rate is missing: rb/rb2610; excluded rate rows: ' +
    'investor rb2610: investor group G2');
});
test('the investor group is queried once per sync across products', async () => {
  stub([own('rb', .11, { investorRange: '2', investorId: 'G1' }),
    own('hc', .13, { investorRange: '2', investorId: 'G1' })]);
  assert.equal(await td.syncMarginRates(['rb2610', 'hc2610'], '1'), 2);
  assert.deepEqual(td.readMarginRates(['rb2610', 'hc2610'], '1').map(r => r.longMarginRatioByMoney), [.11, .13]);
  assert.equal(investorQueries.length, 1);
});
test('single investor overrides investor group, which overrides all investors', async () => {
  const all = own('rb2610', .09, { investorRange: '1', investorId: '00000000' });
  const group = own('rb2610', .11, { investorRange: '2', investorId: 'G1' });
  const single = own('rb2610', .13);
  assert.equal(await longRate([single, group, all]), .13);
  assert.equal(await longRate([all, group, single]), .13);
  assert.equal(await longRate([group, all]), .11);
  assert.equal(await longRate([all, group]), .11);
});
test('a contract row still overrides a product row of a narrower range', async () =>
  assert.equal(await longRate([own('rb', .1), own('rb2610', .2, { investorRange: '1' })]), .2));
test('disagreeing rows of one range fail the sync instead of taking the last', async () => {
  await failsWith([own('rb2610', .12), own('rb2610', .04)], [],
    'CTP investor margin rate rb2610 (range 3) has conflicting rows');
  await failsWith([own('rb2610', .12, { investorRange: '1' }), own('rb2610', .12, { investorRange: '1', isRelative: 1 })], [],
    'CTP investor margin rate rb2610 (range 1) has conflicting rows');
  assert.equal(await longRate([own('rb2610', .12), own('rb2610', .12)]), .12, 'identical duplicates agree');
});
test('an unavailable investor group fails the sync', async () => {
  const groupRow = [own('rb2610', .15, { investorRange: '2', investorId: 'G1' })];
  try {
  for (const reply of [
    [{ brokerId: '9999', investorId: 'other', investorGroupId: 'G1' }],
    [{ brokerId: '8888', investorId: 'u', investorGroupId: 'G1' }],
    [{ brokerId: '9999', investorId: 'u', investorGroupId: '' }],
    [{ brokerId: '9999', investorId: 'u', investorGroupId: 'G1' }, { brokerId: '9999', investorId: 'u', investorGroupId: 'G2' }],
  ]) {
    investorReply = reply;
    await failsWith(groupRow, [], 'CTP investor group is unavailable for 9999/u');
  }
  investorReply = [];
  await failsWith(groupRow, [], 'CTP query returned no rows after 5 attempts');
  } finally {
    investorReply = [{ brokerId: '9999', investorId: 'u', investorGroupId: 'G1' }];
  }
});
test('rows scoped to an investment unit never apply to an account without one', async () => {
  await failsWith([own('rb2610', .12, { investUnitId: 'U1' }), own('rb2610', .12, { investorRange: '9' })], [],
    'CTP product/contract rate is missing: rb/rb2610; excluded rate rows: ' +
    'investor rb2610: investment unit U1; investor rb2610: investor range "9"');
  // Single-character codes are quoted, so an empty or blank range is still visible.
  await failsWith([own('rb2610', .12, { investorRange: '' }), own('rb2610', .12, { investorRange: ' ' })], [],
    'CTP product/contract rate is missing: rb/rb2610; excluded rate rows: ' +
    'investor rb2610: investor range ""; investor rb2610: investor range " "');
});
test('rows of another broker are excluded with a diagnostic', async () =>
  failsWith([own('rb2610', .04, { brokerId: '8888' })], [],
    'CTP product/contract rate is missing: rb/rb2610; excluded rate rows: ' +
    'investor rb2610: broker 8888'));
test('exchange base rows of another broker or in conflict fail the sync', async () => {
  const relative = [own('rb2610', .02, { isRelative: 1 })];
  await failsWith(relative, [base('rb2610', .04, { brokerId: '8888' })],
    'CTP exchange margin rate is missing: rb2610/1; excluded rate rows: ' +
    'exchange rb2610: broker 8888');
  await failsWith(relative, [base('rb2610', .1), base('rb2610', .04)],
    'CTP exchange margin rate rb2610 has conflicting rows');
});
test('an investor-group row cannot be judged without the account group', () => {
  assert.throws(() => selectCtpInvestorMarginRates(
    [own('rb2610', .1, { investorRange: '2', investorId: 'G1' })], { brokerId: '9999', investorId: 'u' }, rbRequest),
  { message: 'CTP investor group is required to judge an investor-group rate row' });
});

test('review: an investor row of another exchange is excluded, the own 12% stays', async () => {
  assert.equal(await longRate([own('rb2610', .12), own('rb2610', .04, { exchangeId: 'DCE' })]), .12);
  await failsWith([own('rb2610', .04, { exchangeId: 'DCE' })], [],
    'CTP product/contract rate is missing: rb/rb2610; excluded rate rows: investor rb2610: exchange DCE');
});
test('review: a relative 2% adds only the base of the contract exchange', async () => {
  const relative = [own('rb2610', .02, { isRelative: 1 })];
  await failsWith(relative, [base('rb2610', .04, { exchangeId: 'DCE' })],
    'CTP exchange margin rate is missing: rb2610/1; excluded rate rows: exchange rb2610: exchange DCE');
  const rate = await longRate(relative, [base('rb2610', .04, { exchangeId: 'DCE' }), base('rb2610', .1)]);
  assert(Math.abs(rate - .12) < 1e-12, `expected 0.12, got ${rate}`);
});
test('rows without an ExchangeID are judged by their key alone', async () => {
  assert.equal(await longRate([own('rb2610', .1, { exchangeId: '' })]), .1);
  const rate = await longRate([own('rb2610', .02, { isRelative: 1 })], [base('rb2610', .1, { exchangeId: '' })]);
  assert(Math.abs(rate - .12) < 1e-12, `expected 0.12, got ${rate}`);
});
test('review: a group row of another broker, unit or exchange does not trigger the group lookup', async () => {
  investorReply = [];
  try {
    for (const foreign of [{ brokerId: '8888' }, { investUnitId: 'U1' }, { exchangeId: 'DCE' }])
      assert.equal(await longRate([own('rb2610', .12), own('rb2610', .04, { investorRange: '2', investorId: 'G9', ...foreign })]), .12);
    assert.equal(investorQueries.length, 0);
  } finally {
    investorReply = [{ brokerId: '9999', investorId: 'u', investorGroupId: 'G1' }];
  }
  const account = { brokerId: '9999', investorId: 'u' };
  const group = own('rb2610', .1, { investorRange: '2', investorId: 'G1' });
  const needsGroup = (rows, request = rbRequest) => ctpInvestorGroupNeeded(rows, account, request, 'margin');
  assert.equal(needsGroup([group]), true);
  assert.equal(ctpInvestorGroupNeeded([group], { ...account, investorGroupId: 'G1' }, rbRequest, 'margin'), false);
  assert.throws(() => ctpInvestorGroupNeeded([group], account, 'SHFE', 'margin'), /expected instrumentIds/);
  assert.equal(needsGroup([{ ...group, exchangeId: '' }]), true);
  assert.equal(needsGroup([own('rb2610', .1)]), false);
  assert.equal(needsGroup([group, own('rb2610', .12)]), false);
  assert.equal(needsGroup([group, own('rb2610', .12), { ...group, instrumentId: 'rb2701' }],
    { ...rbRequest, instrumentIds: ['rb2610', 'rb2701'] }), true);
  assert.equal(needsGroup([group, own('rb2610', .12, { investorId: 'other' })]), true);
  assert.equal(await longRate([group, own('rb2610', .12)]), .12);
  assert.equal(investorQueries.length, 0);
});
test('review: a contract listed under two exchanges fails instead of following row order', async () => {
  try {
    for (const order of [['SHFE', 'DCE'], ['DCE', 'SHFE']]) {
      td.reqQryInstrument = async () => order.map(exchangeId => ({ instrumentId: 'rb2610', productId: 'rb', exchangeId, volumeMultiple: 10 }));
      stub([own('rb2610', .12), own('rb2610', .04, { exchangeId: 'DCE' })]);
      await assert.rejects(td.syncMarginRates(['rb2610'], '1'),
        { message: 'CTP instrument identity conflicts in the catalog: rb2610' });
    }
  } finally {
    td.reqQryInstrument = async () => shfeCatalog;
  }
});
test('review: rows of an unrequested month neither trigger the group lookup nor fail the sync', async () => {
  investorReply = [];
  try {
    assert.equal(await longRate([own('rb2610', .12), own('rb2701', .04, { investorRange: '2', investorId: 'G1' })]), .12);
    assert.equal(await longRate([own('rb2610', .12), own('rb2701', .04), own('rb2701', .05)]), .12);
    assert.equal(investorQueries.length, 0);
  } finally {
    investorReply = [{ brokerId: '9999', investorId: 'u', investorGroupId: 'G1' }];
  }
  assert.equal(await longRate([own('rb', .11, { investorRange: '2', investorId: 'G1' }), own('rb2701', .2)]), .11,
    'the product row still applies to the requested contract');
});
test('a product needs exactly one catalog exchange', async () => {
  try {
  for (const catalog of [
    [{ instrumentId: 'rb2610', productId: 'rb', exchangeId: 'SHFE' }, { instrumentId: 'rb2701', productId: 'rb', exchangeId: 'DCE' },
      { instrumentId: 'rb2705', productId: 'rb', exchangeId: 'SHFE' }],
    [{ instrumentId: 'rb2610', productId: 'rb', exchangeId: 'SHFE' }, { instrumentId: 'rb2701', productId: 'rb', exchangeId: 'DCE' }],
    [{ instrumentId: 'rb2610', productId: 'rb', exchangeId: '' }, { instrumentId: 'rb2701', productId: 'rb', exchangeId: '' }],
  ]) {
    td.reqQryInstrument = async () => catalog;
    stub([own('rb', .1)]);
    await assert.rejects(td.syncMarginRates(['rb2610', 'rb2701'], '1'),
      { message: 'CTP product rb has no single exchange in the instrument catalog' });
    await assert.rejects(td.syncMarginRates(['rb2610'], '1'),
      { message: 'CTP product rb has no single exchange in the instrument catalog' },
      'an unrequested month of another exchange makes the product ambiguous too');
  }
  } finally {
    td.reqQryInstrument = async () => shfeCatalog;
  }
});

test('SimNow ao commission: the captured personal product row prices ao2701', () => {
  const request = { productId: 'ao', exchangeId: 'SHFE', instrumentIds: ['ao2701'] };
  const account = { brokerId: '9999', investorId: '024379' };
  const { rates, excluded } = selectCtpInvestorRates([simnowAo], account, request,
    ['openRatioByMoney', 'openRatioByVolume'], 'commission');
  assert.equal(selectCtpProductRate(rates, 'ao2701', 'ao', excluded), simnowAo);
  assert.deepEqual(excluded, []);
});

test('selectCtpInvestorRates: commission rows follow the margin attribution and business type', async () => {
  const COMMISSION = ['openRatioByVolume'];
  const fee = (perLot, extra = {}) => ({ instrumentId: 'rb2610', exchangeId: 'SHFE', investorRange: '3', brokerId: '9999',
    investorId: 'u', investUnitId: '', bizType: '1', openRatioByVolume: perLot, ...extra });
  const account = { brokerId: '9999', investorId: 'u', investorGroupId: 'G1' };
  const pick = rows => selectCtpInvestorRates(rows, account, rbRequest, COMMISSION, 'commission');
  assert.equal(pick([fee(2)]).rates.get('rb2610').openRatioByVolume, 2);
  // The review's two cases: a securities-business row, and a group row whose
  // group id happens to equal the investor id while the account's group differs.
  for (const [stray, reason] of [[fee(7, { bizType: '2' }), 'business type "2"'], [fee(7, { bizType: '9' }), 'business type "9"'],
    [fee(7, { bizType: ' ' }), 'business type " "'], [fee(11, { investorRange: '2', investorId: 'u' }), 'investor group u']]) {
    const alone = pick([stray]);
    assert.equal(alone.rates.size, 0);
    assert.deepEqual(alone.excluded, [`investor rb2610: ${reason}`]);
    assert.equal(pick([stray, fee(2)]).rates.get('rb2610').openRatioByVolume, 2);
  }
  for (const bizType of ['', undefined, null]) {
    const personal = fee(7, { bizType });
    assert.equal(pick([personal, fee(2, { investorRange: '1' })]).rates.get('rb2610'), personal);
    assert.throws(() => pick([personal, fee(2)]), /commission rate rb2610 \(range 3\) has conflicting rows/);
    assert.deepEqual(pick([{ ...personal, investorId: 'other' }]).excluded, ['investor rb2610: investor other']);
  }
  assert.equal(ctpInvestorGroupNeeded([fee(7, { bizType: '', investorRange: '2', investorId: 'G1' })],
    { brokerId: '9999', investorId: 'u' }, rbRequest, 'commission'), true);
  assert.throws(() => selectCtpInvestorRates([], account, rbRequest, COMMISSION, 'unknown'), TypeError);
  // Narrowest range wins: single over the account's group over all investors (wildcard id).
  assert.equal(pick([fee(5, { investorRange: '1', investorId: '00000000' }), fee(4, { investorRange: '2', investorId: 'G1' })])
    .rates.get('rb2610').openRatioByVolume, 4);
  assert.equal(pick([fee(5, { investorRange: '1', investorId: '00000000' })]).rates.get('rb2610').openRatioByVolume, 5);
  assert.throws(() => pick([fee(2), fee(3)]), { message: 'CTP commission rate rb2610 (range 3) has conflicting rows' });
  assert.throws(() => selectCtpInvestorRates([fee(4, { investorRange: '2', investorId: 'G1' })], { brokerId: '9999', investorId: 'u' },
    rbRequest, COMMISSION, 'commission'), { message: 'CTP investor group is required to judge an investor-group rate row' });
  assert.equal(ctpInvestorGroupNeeded([fee(4, { investorRange: '2', investorId: 'G1', bizType: '2' })], account, rbRequest, 'commission'), false,
    'a securities-business group row needs no group lookup');
});

const groupRate = (key, value, group = 'G1') => own(key, value, { investorRange: '2', investorId: group });
test('a rejected investor lookup matters only when the effective rate needs it', async () => {
  const queryInvestor = td.reqQryInvestor;
  let calls = 0;
  td.reqQryInvestor = async () => { calls++; throw new Error('investor lookup denied'); };
  try {
    assert.equal(await longRate([own('rb2610', .12), groupRate('rb', .09)]), .12);
    assert.equal(calls, 0);
    await failsWith([groupRate('rb', .09)], [], 'investor lookup denied');
    assert.equal(calls, 1);
  } finally {
    td.reqQryInvestor = queryInvestor;
  }
});
test('an unmatched contract group exposes conflicting applicable product rows', async () => {
  await failsWith([groupRate('rb2610', .13, 'G2'), own('rb', .09), own('rb', .08)], [],
    'CTP investor margin rate rb (range 3) has conflicting rows');
  assert.equal(investorQueries.length, 1);
});
for (const [name, rows, expected, queries] of [
  ['personal contract covers the product group', [own('rb2610', .12), groupRate('rb', .09)], [.12], 0],
  ['all-investor contract covers the product group', [groupRate('rb', .09), own('rb2610', .12, { investorRange: '1' })], [.12], 0],
  ['another requested month needs the product group', [own('rb2610', .12), groupRate('rb', .09)], [.12, .09], 1],
  ['a contract with only a group still needs identification', [own('rb', .09), groupRate('rb2610', .13)], [.13], 1],
  ['a foreign contract group leaves the product applicable', [own('rb', .09), groupRate('rb2610', .13, 'G2')], [.09], 1],
  ['a foreign personal contract leaves the product group applicable', [own('rb2610', .12, { investorId: 'other' }), groupRate('rb', .09)], [.09], 1],
  ['a confirmed contract group supersedes conflicting product rows', [groupRate('rb2610', .13), own('rb', .09), own('rb', .08)], [.13], 1],
  ['the contract group can override its own all-investor row', [own('rb2610', .12, { investorRange: '1' }), groupRate('rb2610', .13), groupRate('rb', .09)], [.13], 1],
]) {
  test(`cross-key margin: ${name}`, async () => {
    const ids = ['rb2610', 'rb2701'].slice(0, expected.length);
    stub(rows);
    assert.equal(await td.syncMarginRates(ids, '1'), ids.length);
    assert.deepEqual(td.readMarginRates(ids, '1').map(rate => rate.longMarginRatioByMoney), expected);
    assert.equal(investorQueries.length, queries);
  });
}

test('a missing-rate error names at most ten excluded rows and counts the rest', async () => {
  const excluded = Array.from({ length: 13 }, (_, i) => `investor rb26${10 + i}: broker 8888`);
  assert.equal(ctpMarginRateMissing('product/contract', 'rb/rb2610', excluded).message,
    `CTP product/contract rate is missing: rb/rb2610; excluded rate rows: ${excluded.slice(0, 10).join('; ')}; and 3 more`);
  assert.equal(ctpMarginRateMissing('product/contract', 'rb/rb2610', excluded.slice(0, 10)).message,
    `CTP product/contract rate is missing: rb/rb2610; excluded rate rows: ${excluded.slice(0, 10).join('; ')}`);
  assert.equal(ctpMarginRateMissing('exchange margin', 'rb2610/1', []).message, 'CTP exchange margin rate is missing: rb2610/1');
});

let failed = 0;
for (const [name, fn] of tests) {
  try { await fn(); console.log(`  PASS: ${name}`); }
  catch (error) { failed++; console.log(`  FAIL: ${name}\n    ${error.message}`); }
}
td.close();
if (failed) {
  console.log(`product rate tests: ${failed} failed`);
  process.exit(1);
}
console.log('product rate tests passed');
