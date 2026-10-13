import { test } from 'node:test';
import assert from 'node:assert/strict';
import { writeStv2, readStv2, crc32, keyToken, indexToken, rgb565, S, OP, NONE, Stv2Error } from '../src/format.js';

// The smallest valid theme: one screen with one rect.
function minimal(extra = {}) {
  return {
    sections: {
      [S.SCREENS]: [{ firstOp: 0, opCount: 1, seconds: 0, background: 0 }],
      [S.OPS]: [{ type: OP.RECT, x: 10, y: 10, w: 20, h: 20, color: 0xf800 }],
      ...extra,
    },
    blob: new Uint8Array(0),
  };
}
// Rewrites the CRC after a deliberate corruption so the structural check runs.
function recrc(bytes) {
  new DataView(bytes.buffer).setUint32(12, crc32(bytes, 16), true);
  return bytes;
}

test('crc32 and tokens match known values', () => {
  assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
  assert.equal(keyToken('daily'), keyToken('daily'));
  assert.notEqual(keyToken('daily'), keyToken('current'));
  assert.equal(keyToken('a') & 0x80000000, 0);
  assert.equal(indexToken(3), 0x80000003);
  assert.equal(rgb565('#ff0000'), 0xf800);
  assert.equal(rgb565('#ffffff'), 0xffff);
  assert.throws(() => rgb565('red'));
});

test('write then read round-trips records', () => {
  const bytes = writeStv2(minimal());
  assert.equal(bytes.length % 4, 0);
  const theme = readStv2(bytes);
  assert.equal(theme.name, NONE);
  assert.deepEqual(theme.sections[S.OPS][0], { type: OP.RECT, flags: 0, x: 10, y: 10, w: 20, h: 20, color: 0xf800, color2: 0, a: 0, b: 0, c: 0 });
  assert.equal(theme.sections[S.IMAGES].length, 0);
});

test('reader rejects damaged or inconsistent files', () => {
  const good = writeStv2(minimal());
  assert.throws(() => readStv2(good.subarray(0, 20)), Stv2Error);
  const flipped = good.slice(); flipped[40] ^= 1;
  assert.throws(() => readStv2(flipped), /checksum/);
  const magic = good.slice(); magic[0] = 0x58;
  assert.throws(() => readStv2(magic), /not an STV2/);
  // An op pointing past the end of the ops section.
  const badRef = writeStv2(minimal({ [S.SCREENS]: [{ firstOp: 0, opCount: 2, seconds: 0, background: 0 }] }));
  assert.throws(() => readStv2(badRef), /range out of bounds/);
  // A text op referencing a font that does not exist.
  assert.throws(() => readStv2(writeStv2(minimal({ [S.OPS]: [{ type: OP.TEXT, a: 0, b: 0 }] }))), /text font/);
  // Unknown op type, after fixing the CRC.
  const unknown = writeStv2(minimal());
  const opAt = new DataView(unknown.buffer).getUint32(32 + 12 + 4, true);
  unknown[opAt] = 9;
  assert.throws(() => readStv2(recrc(unknown)), /op type 9/);
  // No screens at all.
  assert.throws(() => readStv2(writeStv2({ sections: {}, blob: new Uint8Array(0) })), /no screens/);
});
