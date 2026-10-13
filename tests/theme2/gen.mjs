// Writes the fixtures tests/theme2/test_stv2.cpp checks the device core
// against: compiled themes, the JS reference frames, the slot values the JS
// side derives, and damaged files the reader must reject.
// Usage: node tests/theme2/gen.mjs <out-dir>
import { mkdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { compileTheme } from '../../editor/src/compile.js';
import { readStv2, crc32, S } from '../../editor/src/format.js';
import { renderScreen, resolveText } from '../../editor/src/render.js';
import { slotValues, applySource } from '../../editor/src/values.js';
import { source, assets, forecast } from '../../editor/test/sample.js';
import { blockFont, stripes, noise, solidImage } from '../../editor/test/fixtures.js';

const out = process.argv[2];
mkdirSync(out, { recursive: true });
const date = new Date(2026, 9, 13, 9, 5, 7);
const lines = [];

// Values as C++ reads them: "<slot> n <number>", "<slot> s <hex utf-8>", "<slot> -".
const valueLines = (values) => values.map((v, i) => v === undefined ? `${i} -` : typeof v === 'number' ? `${i} n ${v}` : `${i} s ${Buffer.from(v).toString('hex') || '-'}`);

function emit(name, bytes, values, timeMs) {
  const theme = readStv2(bytes);
  writeFileSync(join(out, `${name}.stv`), bytes);
  writeFileSync(join(out, `${name}.values`), valueLines(values).join('\n') + '\n');
  const texts = theme.sections[S.OPS].filter((op) => op.type === 4).map((op) => Buffer.from(resolveText(theme, op.b, values)).toString('hex') || '-');
  writeFileSync(join(out, `${name}.texts`), texts.join('\n') + '\n');
  theme.sections[S.SCREENS].forEach((_, i) => {
    const fb = renderScreen(theme, i, values, timeMs);
    writeFileSync(join(out, `${name}.screen${i}.rgb565`), Buffer.from(fb.buffer));
  });
  lines.push(`${name} ${theme.sections[S.SCREENS].length} ${timeMs}`);
  return theme;
}

// 1. The two-screen weather sample, with values from clock + city + JSON.
const sampleBytes = compileTheme(source, assets());
const sample = readStv2(sampleBytes);
const data = applySource(sample, 0, forecast);
emit('sample', sampleBytes, slotValues(sample, { date, city: 'São José dos Campos', data }), 0);
writeFileSync(join(out, 'forecast.json'), JSON.stringify(forecast, null, 1));
// Same theme before any sync or fetch: everything "--".
emit('sample-empty', sampleBytes, slotValues(sample, { date: null }), 0);

// 2. Shapes, clipping, alignment, raw/alpha/lossy images and animation.
const chars = [...'0123456789:-?ABCabc ãé'].map((c) => c.codePointAt(0));
const shapes = {
  name: 'shapes', background: '#102030',
  fonts: { f: { bpp: 4 }, g: { bpp: 1 } },
  images: { photo: { encoding: 'raw' }, ghost: {}, lossy: { encoding: 'palette' }, anim: { frames: 4 } },
  screens: [{ layers: [
    { type: 'rect', x: -10, y: -10, w: 60, h: 40, color: '#ff0000' },
    { type: 'rect', x: 100, y: 20, w: 60, h: 50, radius: 14, color: '#00ff00' },
    { type: 'rect', x: 200, y: 200, w: 60, h: 60, radius: 120, color: '#ffffff' },
    { type: 'circle', x: 30, y: 60, size: 37, color: '#0000ff' },
    { type: 'circle', x: 225, y: -5, size: 30, color: '#ffff00' },
    { type: 'image', image: 'photo', x: 60, y: 100 },
    { type: 'image', image: 'ghost', x: 70, y: 110 },
    { type: 'image', image: 'lossy', x: 200, y: 100 },
    { type: 'image', image: 'anim', x: 10, y: 200, fps: 3 },
    { type: 'text', font: 'f', x: 0, y: 150, w: 240, align: 'center', color: '#ffffff', text: 'ABC abc ãé 0123' },
    { type: 'text', font: 'g', x: 5, y: 170, w: 50, color: '#ffd166', text: 'abcABCabcABC' },
    { type: 'text', font: 'f', x: 120, y: 185, w: 100, align: 'right', color: '#88ccff', text: '{HH}:{MM}:{SS}' },
    { type: 'text', font: 'f', x: -20, y: 225, w: 240, h: 20, color: '#ffffff', text: 'cut off at the bottom' },
  ] }],
};
const shapesBytes = compileTheme(shapes, {
  fonts: { f: blockFont(chars, { w: 7, h: 11, ascent: 9, advance: 8, lineHeight: 13 }), g: blockFont(chars, { w: 5, h: 7, ascent: 6, advance: 6, lineHeight: 9 }) },
  images: {
    photo: noise(40, 30, 3),
    ghost: solidImage(30, 20, [255, 255, 255, 90]),
    lossy: noise(30, 30, 9),
    anim: stripes(16, 32, [[255, 0, 0, 255], [0, 255, 0, 160], [0, 0, 255, 255], [0, 0, 0, 0]]),
  },
});
const shapesTheme = readStv2(shapesBytes);
emit('shapes', shapesBytes, slotValues(shapesTheme, { date }), 2500);

// 3. Damaged files: each must be rejected.
const damaged = {
  truncated: sampleBytes.subarray(0, sampleBytes.length - 4),
  crc: (() => { const b = sampleBytes.slice(); b[100] ^= 0x40; return b; })(),
  magic: (() => { const b = sampleBytes.slice(); b[0] = 0x41; return b; })(),
  // A palette index past the palette, with the CRC fixed so the image check runs.
  palette: (() => {
    const b = shapesBytes.slice();
    const t = readStv2(b);
    const im = t.sections[S.IMAGES].find((x) => x.encoding === 1);
    const view = new DataView(b.buffer);
    const count = view.getUint16(6, true);
    let blobOffset = 0;
    for (let i = 0; i < count; i++) if (view.getUint16(32 + i * 12, true) === S.BLOB) blobOffset = view.getUint32(32 + i * 12 + 4, true);
    const base = blobOffset + im.offset;
    const n = view.getUint16(base, true);
    const row0 = view.getUint32(base + 4 + n * 3, true);
    b[base + row0 + 1] = 255;   // first data byte of row 0
    view.setUint32(12, crc32(b, 16), true);
    return b;
  })(),
};
for (const [name, bytes] of Object.entries(damaged)) {
  let rejected = false;
  try { readStv2(bytes); } catch { rejected = true; }
  if (!rejected) throw new Error(`JS reader accepted damaged file ${name}`);
  writeFileSync(join(out, `bad-${name}.stv`), bytes);
}
writeFileSync(join(out, 'cases.txt'), lines.join('\n') + '\n');
writeFileSync(join(out, 'bad.txt'), Object.keys(damaged).map((n) => `bad-${n}`).join('\n') + '\n');
console.log(`theme2 fixtures written to ${out}`);
