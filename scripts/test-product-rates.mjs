import assert from 'node:assert/strict';
import { groupCtpInstrumentsByProduct, selectCtpProductRate } from '../dist/index.js';
const catalog = [{ instrumentId: 'rb2610', productId: 'rb' }, { instrumentId: 'rb2701', productId: 'rb' },
  { instrumentId: 'IF2610', productId: 'IF' }];
assert.deepEqual([...groupCtpInstrumentsByProduct(['rb2610', 'rb2701', 'IF2610', 'rb2610'], catalog)],
  [['rb', ['rb2610', 'rb2701']], ['IF', ['IF2610']]]);
for (const productId of [undefined, null, '', ' ', 'rb\0'])
  assert.throws(() => groupCtpInstrumentsByProduct(['rb2610'], [{ instrumentId: 'rb2610', productId }]));
assert.throws(() => groupCtpInstrumentsByProduct(['unknown'], catalog));
const rates = new Map([['rb', .1], ['rb2701', .2], ['rb2710', NaN]]);
assert.equal(selectCtpProductRate(rates, 'rb2610', 'rb'), .1);
assert.equal(selectCtpProductRate(rates, 'rb2701', 'rb'), .2);
assert(Number.isNaN(selectCtpProductRate(rates, 'rb2710', 'rb')));
assert.throws(() => selectCtpProductRate(rates, 'IF2610', 'IF'));
console.log('product rate tests passed');
