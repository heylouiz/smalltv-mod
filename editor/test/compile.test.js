import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseTemplate, parsePath, requiredGlyphs, compileTheme, CompileError } from '../src/compile.js';
import { readStv2, keyToken, indexToken, S, SLOT, SLOT_TYPE, CLOCK } from '../src/format.js';
import { source, assets } from './sample.js';
import { blockFont } from './fixtures.js';

test('templates parse placeholders, formats and escaped braces', () => {
  assert.deepEqual(parseTemplate('{HH}:{MM}'), [{ token: 'HH', format: undefined }, { literal: ':' }, { token: 'MM', format: undefined }]);
  assert.deepEqual(parseTemplate('{{x}} {w.t|1}°'), [{ literal: '{x} ' }, { token: 'w.t', format: '1' }, { literal: '°' }]);
  for (const bad of ['', '{HH', 'a}b', '{}', '{a|}', '{a|b|c}']) assert.throws(() => parseTemplate(bad), CompileError, bad);
});

test('paths become tokens; ranges expand into suffixed fields', () => {
  assert.deepEqual(parsePath('current.temperature_2m'), [{ suffix: '', tokens: [keyToken('current'), keyToken('temperature_2m')] }]);
  const days = parsePath('daily.max[0..2]');
  assert.deepEqual(days.map((d) => d.suffix), ['0', '1', '2']);
  assert.deepEqual(days[2].tokens, [keyToken('daily'), keyToken('max'), indexToken(2)]);
  assert.deepEqual(parsePath('[1].v')[0].tokens, [indexToken(1), keyToken('v')]);
  assert.deepEqual(parsePath('m[1][2]')[0].tokens, [keyToken('m'), indexToken(1), indexToken(2)]);
  for (const bad of ['', 'a..b', 'a[x]', 'a[3..1]', 'a.b.c.d.e.f.g.h.i']) assert.throws(() => parsePath(bad), CompileError, bad);
});

test('required glyphs cover literals, table strings, digits and free text', () => {
  const need = requiredGlyphs(source);
  const has = (set, s) => [...s].every((c) => set.has(c.codePointAt(0)));
  assert.ok(has(need.big, '0123456789:-'));
  assert.ok(has(need.small, 'Terça'), 'weekday names with accents');
  assert.ok(has(need.small, 'Março'), 'month names');
  assert.ok(has(need.small, 'Trovoadas com granizo'), 'weather texts');
  assert.ok(has(need.small, 'São José ãõç'), 'city needs Latin-1');
  assert.ok(!need.big.has('S'.codePointAt(0)), 'the clock font stays small');
});

test('the sample compiles to a valid, deterministic STV2', () => {
  const a = compileTheme(source, assets());
  const b = compileTheme(source, assets());
  assert.deepEqual(a, b);
  const theme = readStv2(a);
  const s = theme.sections;
  assert.equal(s[S.SCREENS].length, 2);
  assert.deepEqual(s[S.SCREENS].map((x) => x.seconds), [15, 10]);
  assert.equal(s[S.SOURCES].length, 1);
  // Only fields some layer reads are fetched: temp, code, max0/1, min0/1.
  assert.equal(s[S.FIELDS].length, 6);
  const kinds = s[S.SLOTS].map((x) => x.kind);
  assert.ok(kinds.includes(SLOT.CITY) && kinds.includes(SLOT.DATA));
  assert.ok(s[S.SLOTS].some((x) => x.kind === SLOT.CLOCK && x.arg === CLOCK.WEEKDAY));
  const city = s[S.SLOTS].find((x) => x.kind === SLOT.CITY);
  assert.equal(city.type, SLOT_TYPE.STRING);
  assert.ok(a.length < 60000, `sample is ${a.length} bytes`);
});

test('compile errors name the layer and the problem', () => {
  const bad = (patch, pattern) => {
    const src = structuredClone(source);
    patch(src);
    assert.throws(() => compileTheme(src, assets()), pattern);
  };
  bad((s) => { s.screens[0].layers[1].text = '{NOPE}'; }, /screens\[0\]\.layers\[1\]: unknown placeholder \{NOPE\}/);
  bad((s) => { s.screens[0].layers[1].text = '{HH|nope}'; }, /unknown format 'nope'/);
  bad((s) => { s.sources.w.url = 'https://api.open-meteo.com/v1/forecast'; }, /http:\/\//);
  bad((s) => { s.screens[0].layers[0].image = 'missing'; }, /unknown image 'missing'/);
  bad((s) => { s.screens[1].layers[0].color = 'blue'; }, /color/);
  bad((s) => { s.screens = []; }, /at least one screen/);
  bad((s) => { s.screens[0].layers.push({ type: 'blink' }); }, /unknown layer type/);
  assert.throws(() => compileTheme(source, { ...assets(), fonts: { big: blockFont([48]) } }), /font 'small' was not rasterized/);
});
