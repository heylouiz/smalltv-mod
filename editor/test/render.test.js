import { test } from 'node:test';
import assert from 'node:assert/strict';
import { compileTheme } from '../src/compile.js';
import { readStv2, rgb565, S, SLOT, WIDTH } from '../src/format.js';
import { renderScreen, resolveText, formatValue, textWidth } from '../src/render.js';
import { slotValues, applySource, sourceUrl } from '../src/values.js';
import { source, assets, forecast } from './sample.js';
import { blockFont, stripes } from './fixtures.js';

const sample = () => readStv2(compileTheme(source, assets()));
const at = (fb, x, y) => fb[y * WIDTH + x];
// Builds a one-screen theme from layers, with a block font for text.
function single(layers, extra = {}) {
  const src = { fonts: { f: { bpp: 4 } }, screens: [{ layers }], ...extra };
  const chars = [...'0123456789:-?ABCabc '].map((c) => c.codePointAt(0));
  return readStv2(compileTheme(src, { fonts: { f: blockFont(chars) }, images: extra.pixels || {} }));
}

test('values: clock, city and the Open-Meteo response fill the slots', () => {
  const theme = sample();
  const date = new Date(2026, 9, 13, 9, 5, 7); // a Tuesday
  const data = applySource(theme, 0, forecast);
  const values = slotValues(theme, { date, city: 'Limeira', data });
  const texts = theme.sections[S.OPS].filter((op) => op.type === 4).map((op) => resolveText(theme, op.b, values));
  assert.deepEqual(texts, ['09:05', 'Terça, 13 de Outubro', 'Limeira', '24.9°C Chuva fraca', 'Hoje', '29° / 17°', 'Qua', '31° / 19°']);
  // Before the first sync and fetch everything reads "--".
  const empty = slotValues(theme, { date: null });
  assert.equal(resolveText(theme, theme.sections[S.OPS][1].b, empty), '--:--');
  assert.equal(resolveText(theme, theme.sections[S.OPS][5].b, empty), '--°C --');
});

test('number formats: rounding, padding, wrap-around and tables', () => {
  const theme = sample();
  const fmt = (match) => theme.sections[S.FORMATS].findIndex(match);
  const two = fmt((f) => f.width === 2);
  assert.equal(formatValue(theme, 7, two), '07');
  assert.equal(formatValue(theme, 12, two), '12');
  const oneDecimal = fmt((f) => f.decimals === 1);
  assert.equal(formatValue(theme, 2.25, oneDecimal), '2.3');
  assert.equal(formatValue(theme, -2.25, oneDecimal), '-2.3');
  assert.equal(formatValue(theme, 0.04, oneDecimal), '0.0');
  const wd1 = fmt((f) => f.mod === 7);
  assert.equal(formatValue(theme, 6, wd1), 'Dom', 'Saturday + 1 wraps to Sunday');
  assert.equal(formatValue(theme, 'texto', two), 'texto', 'strings pass through');
});

test('data: matching paths only, strings truncated on a character boundary', () => {
  const theme = sample();
  const data = applySource(theme, 0, { current: { temperature_2m: 'x', weather_code: 3 }, daily: { temperature_2m_max: [1, 2] }, other: { temperature_2m: 99 } });
  const slots = theme.sections[S.SLOTS];
  const byField = Object.fromEntries(theme.sections[S.FIELDS].map((f) => [f.slot, data[f.slot]]));
  assert.ok(Object.values(byField).includes(3));
  assert.ok(!Object.values(byField).includes('x'), 'a string does not land in a number slot');
  assert.ok(!Object.values(byField).includes(99), 'same key under another parent does not match');
  const cityIndex = slots.findIndex((s) => s.kind === SLOT.CITY);
  const long = slotValues(theme, { city: 'São José dos Campos e mais um nome bem comprido' })[cityIndex];
  assert.ok(new TextEncoder().encode(long).length <= 32);
  assert.ok(!long.includes('�'));
  assert.equal(sourceUrl(theme, 0, null), null, 'no location: skipped');
  assert.match(sourceUrl(theme, 0, { lat: -22.5647, lon: -47.4017 }), /latitude=-22\.5647&longitude=-47\.4017&/);
});

test('rects, rounded corners and circles follow the integer rules', () => {
  const theme = single([
    { type: 'rect', x: 10, y: 10, w: 20, h: 10, color: '#ff0000' },
    { type: 'rect', x: 100, y: 100, w: 40, h: 40, radius: 10, color: '#00ff00' },
    { type: 'circle', x: 200, y: 0, size: 10, color: '#0000ff' },
    { type: 'rect', x: 230, y: 230, w: 20, h: 20, color: '#ffffff' },
  ]);
  const fb = renderScreen(theme, 0);
  assert.equal(at(fb, 10, 10), 0xf800);
  assert.equal(at(fb, 29, 19), 0xf800);
  assert.equal(at(fb, 30, 10), 0, 'w is exclusive');
  assert.equal(at(fb, 100, 100), 0, 'rounded corner is cut');
  assert.equal(at(fb, 103, 103), 0x07e0, 'inside the corner arc');
  assert.equal(at(fb, 120, 100), 0x07e0, 'edge between corners');
  assert.equal(at(fb, 205, 5), 0x001f);
  assert.equal(at(fb, 200, 0), 0, 'circle corner');
  assert.equal(at(fb, 239, 239), 0xffff, 'clipped to the screen');
});

test('text aligns in its box, blends coverage and clips', () => {
  const theme = single([
    { type: 'text', font: 'f', x: 0, y: 0, w: 240, align: 'left', color: '#ffffff', text: 'AB' },
    { type: 'text', font: 'f', x: 0, y: 20, w: 240, align: 'center', color: '#ffffff', text: 'AB' },
    { type: 'text', font: 'f', x: 0, y: 40, w: 240, align: 'right', color: '#ffffff', text: 'AB' },
    { type: 'text', font: 'f', x: 0, y: 60, w: 9, color: '#ffffff', text: 'ABC' },
  ]);
  const font = theme.sections[S.FONTS][0];
  assert.equal(textWidth(theme, font, 'AB'), 14);
  const fb = renderScreen(theme, 0);
  // Block glyph: coverage ramps 1/6..6/6 across x; column 5 is full (255)
  // except on rows the fixture punches out ('A' keeps row 1).
  assert.equal(at(fb, 5, 1), 0xffff);
  assert.ok(at(fb, 0, 0) > 0 && at(fb, 0, 0) < 0xffff, 'partial coverage blends');
  assert.equal(at(fb, 113 + 5, 21), 0xffff, 'centered: (240 - 14) / 2 = 113');
  assert.equal(at(fb, 112, 21), 0, 'nothing left of the pen');
  assert.equal(at(fb, 226 + 5, 41), 0xffff, 'right: 240 - 14 = 226');
  assert.equal(at(fb, 9, 60), 0, 'clipped at the box edge');
  assert.ok(at(fb, 8, 60) > 0);
});

test('image frames: fixed, from a value through a table, and animated', () => {
  const pixels = { strip: stripes(4, 12, [[255, 0, 0, 255], [255, 0, 0, 255], [255, 0, 0, 255], [0, 255, 0, 255], [0, 255, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255], [0, 0, 255, 255], [0, 0, 255, 255], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]) };
  const theme = single([
    { type: 'image', image: 'strip', x: 0, y: 0, frame: 1 },
    { type: 'image', image: 'strip', x: 10, y: 0, frame: { value: 'CITY', table: 'wmoIcon' } },
    { type: 'image', image: 'strip', x: 20, y: 0, fps: 2 },
    { type: 'image', image: 'strip', x: 30, y: 0, frame: 3 },
  ], { images: { strip: { frames: 4 } }, pixels });
  const fb = renderScreen(theme, 0, [], 1000);
  assert.equal(at(fb, 0, 0), rgb565('#00ff00'), 'frame 1');
  assert.equal(at(fb, 10, 0), rgb565('#00ff00'), 'no value: the table fallback (1)');
  assert.equal(at(fb, 20, 0), rgb565('#0000ff'), '1 s at 2 fps: frame 2');
  assert.equal(at(fb, 30, 0), 0, 'transparent frame leaves the background');
});

test('the sample renders both screens without errors', () => {
  const theme = sample();
  const values = slotValues(theme, { date: new Date(2026, 9, 13, 9, 5), city: 'Limeira', data: applySource(theme, 0, forecast) });
  const one = renderScreen(theme, 0, values);
  const two = renderScreen(theme, 1, values);
  assert.notDeepEqual(one, two);
  assert.equal(at(two, 0, 0), 0, 'screen background overrides the theme one');
  assert.equal(at(two, 10, 10), rgb565('#102030'));
});
