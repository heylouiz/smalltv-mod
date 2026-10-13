import { test } from 'node:test';
import assert from 'node:assert/strict';
import { rleRow, encodeImage, splitPixels } from '../src/images.js';
import { ENC } from '../src/format.js';
import { imageRow } from '../src/render.js';
import { solidImage, stripes, noise } from './fixtures.js';

// Decodes an RLE row per the spec, independently of the renderer.
function unrle(bytes, width) {
  const out = [];
  let at = 0;
  while (out.length < width) {
    const c = bytes[at++];
    if (c < 128) for (let i = 0; i <= c; i++) out.push(bytes[at++]);
    else { const v = bytes[at++]; for (let i = 0; i < c - 126; i++) out.push(v); }
  }
  assert.equal(at, bytes.length, 'row consumed exactly');
  return out;
}

test('RLE rows round-trip, including long runs and literal blocks', () => {
  let s = 7;
  const rand = () => (s = (Math.imul(s, 1103515245) + 12345) >>> 0) >>> 29;
  const rows = [
    new Uint8Array(240).fill(3),
    Uint8Array.from({ length: 240 }, (_, i) => i % 256),
    Uint8Array.from({ length: 240 }, () => rand()),
    Uint8Array.from([1]),
    Uint8Array.from([1, 2]),
    Uint8Array.from([5, 5]),
  ];
  for (const row of rows) assert.deepEqual(unrle(rleRow(row), row.length), [...row]);
  assert.ok(rleRow(new Uint8Array(240).fill(3)).length <= 4, 'a flat row compresses to a couple of runs');
});

// Decodes an encoded image through the renderer's row reader.
function decode(enc) {
  const theme = { blob: enc.data };
  const im = { ...enc, offset: 0, size: enc.data.length };
  const colors = new Uint16Array(im.w), alpha = new Uint8Array(im.w);
  const out = [];
  for (let f = 0; f < im.frames; f++) for (let r = 0; r < im.h; r++) {
    imageRow(theme, im, f, r, colors, alpha);
    out.push([...colors], [...alpha]);
  }
  return out;
}

test('few-color images are stored as lossless palette-RLE', () => {
  const img = stripes(40, 30, [[255, 0, 0, 255], [0, 0, 255, 255], [0, 0, 0, 0]]);
  const enc = encodeImage(img, { frames: 3 });
  assert.equal(enc.encoding, ENC.PAL_RLE);
  assert.equal(enc.lossy, false);
  assert.equal(enc.h, 10);
  const { color, alpha } = splitPixels(img.data);
  const rows = decode(enc);
  for (let y = 0; y < 30; y++) {
    assert.deepEqual(rows[y * 2], [...color.subarray(y * 40, y * 40 + 40)]);
    assert.deepEqual(rows[y * 2 + 1], [...alpha.subarray(y * 40, y * 40 + 40)]);
  }
  assert.ok(enc.data.length < 40 * 30 * 2 / 4, 'much smaller than raw');
});

test('raw RGB565 is kept when it is smaller or exact matters', () => {
  const solid = encodeImage(solidImage(8, 8, [10, 200, 30]), { mode: 'raw' });
  assert.equal(solid.encoding, ENC.RGB565);
  assert.equal(solid.data.length, 8 * 8 * 2);
  const translucent = encodeImage(solidImage(4, 4, [10, 200, 30, 128]), { mode: 'raw' });
  assert.equal(translucent.flags, 1);
  assert.equal(translucent.data.length, 4 * 4 * 3);
  // Noise has thousands of colors: quantizing loses detail without halving
  // the size, so auto keeps it raw.
  const photo = encodeImage(noise(64, 64));
  assert.equal(photo.encoding, ENC.RGB565);
  const forced = encodeImage(noise(64, 64), { mode: 'palette' });
  assert.equal(forced.lossy, true);
  assert.equal(new DataView(forced.data.buffer).getUint16(0, true), 256);
  assert.throws(() => encodeImage(solidImage(4, 5, [0, 0, 0]), { frames: 2 }), /multiple/);
});
