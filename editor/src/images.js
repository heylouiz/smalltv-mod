// images.js — turns RGBA pixels into an STV2 image payload: palette-RLE when
// the image has (or can be reduced to) 256 colors, raw RGB565 otherwise or
// when that is smaller. Pure JS so it runs in the browser and under Node.
import { ENC } from './format.js';

const to565 = (r, g, b) => ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

// RGBA (Uint8Array/ClampedArray, w*h*4) -> parallel RGB565 + alpha arrays.
// Fully transparent pixels collapse to color 0 so they share one palette slot.
export function splitPixels(rgba) {
  const n = rgba.length / 4;
  const color = new Uint16Array(n), alpha = new Uint8Array(n);
  for (let i = 0; i < n; i++) {
    const a = rgba[i * 4 + 3];
    alpha[i] = a;
    color[i] = a ? to565(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]) : 0;
  }
  return { color, alpha };
}

// Median cut over the distinct (color, alpha) pairs, weighted by pixel count.
// Returns a palette of at most `max` entries and the index of every pixel.
export function quantize(color, alpha, max = 256) {
  const counts = new Map();
  for (let i = 0; i < color.length; i++) {
    const key = alpha[i] * 65536 + color[i];
    counts.set(key, (counts.get(key) || 0) + 1);
  }
  let entries = [...counts].map(([key, n]) => {
    const c = key & 0xffff;
    return { key, n, v: [(c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3, key >>> 16] };
  });
  // Few enough colors: each one is its own palette entry (lossless).
  let boxes = entries.length <= max ? entries.map((e) => [e]) : [entries];
  if (entries.length > max) {
    while (boxes.length < max) {
      // Split the box with the widest channel range (ties: most pixels).
      let best = -1, bestSpan = -1, bestAxis = 0;
      boxes.forEach((box, i) => {
        if (box.length < 2) return;
        for (let axis = 0; axis < 4; axis++) {
          let lo = 255, hi = 0;
          for (const e of box) { lo = Math.min(lo, e.v[axis]); hi = Math.max(hi, e.v[axis]); }
          if (hi - lo > bestSpan) { bestSpan = hi - lo; best = i; bestAxis = axis; }
        }
      });
      if (best < 0 || bestSpan === 0) break;
      const box = boxes[best].sort((a, b) => a.v[bestAxis] - b.v[bestAxis] || a.key - b.key);
      const total = box.reduce((s, e) => s + e.n, 0);
      let acc = 0, cut = 1;
      for (let i = 0; i < box.length - 1; i++) { acc += box[i].n; if (acc * 2 >= total) { cut = i + 1; break; } }
      boxes.splice(best, 1, box.slice(0, cut), box.slice(cut));
    }
  }
  const palette = [], paletteAlpha = [], lookup = new Map();
  for (const box of boxes) {
    const w = box.reduce((s, e) => s + e.n, 0);
    const avg = [0, 1, 2, 3].map((k) => Math.round(box.reduce((s, e) => s + e.v[k] * e.n, 0) / w));
    const index = palette.length;
    palette.push(avg[3] ? to565(avg[0], avg[1], avg[2]) : 0);
    paletteAlpha.push(avg[3]);
    for (const e of box) lookup.set(e.key, index);
  }
  const indices = new Uint8Array(color.length);
  for (let i = 0; i < color.length; i++) indices[i] = lookup.get(alpha[i] * 65536 + color[i]);
  return { palette, paletteAlpha, indices, exact: entries.length <= max };
}

// One row of indices -> RLE bytes (see the spec): runs of 2+ equal bytes become
// repeats (c >= 128), everything else literal blocks of up to 128.
export function rleRow(row) {
  const out = [];
  let i = 0;
  while (i < row.length) {
    let run = 1;
    while (i + run < row.length && row[i + run] === row[i] && run < 129) run++;
    if (run >= 2) { out.push(run + 126, row[i]); i += run; continue; }
    const start = i;
    while (i < row.length && i - start < 128) {
      if (i + 1 < row.length && row[i + 1] === row[i]) break;
      i++;
    }
    out.push(i - start - 1, ...row.subarray(start, i));
  }
  return out;
}

// Encodes RGBA pixels (width x height, frames stacked vertically) into an
// STV2 image. `mode`: 'auto' (smallest of the two), 'palette' or 'raw'.
// Returns { w, h, frames, encoding, flags, data: Uint8Array, lossy }.
export function encodeImage({ width, height, data }, { frames = 1, mode = 'auto' } = {}) {
  if (height % frames) throw new Error('image height must be a multiple of the frame count');
  const { color, alpha } = splitPixels(data);
  const hasAlpha = alpha.some((a) => a !== 255);
  const h = height / frames;
  const raw = () => {
    const px = width * height;
    const out = new Uint8Array(px * 2 + (hasAlpha ? px : 0));
    const v = new DataView(out.buffer);
    for (let i = 0; i < px; i++) v.setUint16(i * 2, color[i], true);
    if (hasAlpha) out.set(alpha, px * 2);
    return { w: width, h, frames, encoding: ENC.RGB565, flags: hasAlpha ? 1 : 0, data: out, lossy: false };
  };
  if (mode === 'raw') return raw();
  const q = quantize(color, alpha);
  const n = q.palette.length;
  const rows = [];
  for (let r = 0; r < height; r++) rows.push(rleRow(q.indices.subarray(r * width, (r + 1) * width)));
  const table = 4 + n * 3;
  let size = table + height * 4;
  const offsets = rows.map((row) => { const at = size; size += row.length; return at; });
  const out = new Uint8Array(size);
  const v = new DataView(out.buffer);
  v.setUint16(0, n, true);
  q.palette.forEach((c, i) => v.setUint16(4 + i * 2, c, true));
  q.paletteAlpha.forEach((a, i) => v.setUint8(4 + n * 2 + i, a));
  offsets.forEach((at, r) => { v.setUint32(table + r * 4, at, true); out.set(rows[r], at); });
  const pal = { w: width, h, frames, encoding: ENC.PAL_RLE, flags: 0, data: out, lossy: !q.exact };
  if (mode === 'palette') return pal;
  const r = raw();
  // Lossless palette always wins when smaller; a lossy one only when it saves
  // at least half, since raw keeps the photo exact.
  return pal.data.length < r.data.length && (q.exact || pal.data.length * 2 <= r.data.length) ? pal : r;
}
