// render.js — the reference renderer. It draws a validated STV2 theme exactly
// as the device does (src/features/theme2/Stv2Render.cpp): same integer
// geometry, same blending, same text layout. The browser preview uses it, and
// the parity tests compare the device renderer against it pixel by pixel.
import { S, OP, OPF, PART, SLOT_TYPE, ENC, NONE, WIDTH, HEIGHT, LIMITS } from './format.js';

const decoder = new TextDecoder();
export const stringAt = (theme, index) => {
  const s = theme.sections[S.STRINGS][index];
  return decoder.decode(theme.blob.subarray(s.offset, s.offset + s.length));
};

// A table maps an integer to its first matching entry's value, else fallback.
export function lookup(theme, tableIndex, n) {
  const t = theme.sections[S.TABLES][tableIndex];
  for (let i = t.firstEntry; i < t.firstEntry + t.count; i++) {
    const e = theme.sections[S.ENTRIES][i];
    if (e.from <= n && n <= e.to) return e.value;
  }
  return t.fallback;
}

// Rounds half away from zero at `decimals` places, as the device does.
const roundTo = (v, decimals) => {
  const p = 10 ** decimals;
  return (Math.sign(v) * Math.floor(Math.abs(v) * p + 0.5)) / p;
};

export function formatValue(theme, value, formatIndex) {
  if (value === undefined || value === null) return '--';
  if (typeof value === 'string' || formatIndex === NONE) return String(value);
  const f = theme.sections[S.FORMATS][formatIndex];
  let n = roundTo(value + f.add, f.decimals);
  if (f.mod) n = ((Math.round(n) % f.mod) + f.mod) % f.mod;
  if (f.table !== NONE) {
    const hit = lookup(theme, f.table, Math.round(n));
    if (hit !== NONE) return stringAt(theme, hit);
  }
  const text = Math.abs(n).toFixed(f.decimals);
  const dot = text.indexOf('.');
  const digits = dot < 0 ? text.length : dot;
  return (n < 0 ? '-' : '') + '0'.repeat(Math.max(0, f.width - digits)) + text;
}

// Truncates to LIMITS.text bytes of UTF-8 without splitting a character.
function clip(text) {
  const bytes = new TextEncoder().encode(text);
  if (bytes.length <= LIMITS.text) return text;
  let end = LIMITS.text;
  while (end > 0 && (bytes[end] & 0xc0) === 0x80) end--;
  return decoder.decode(bytes.subarray(0, end));
}

export function resolveText(theme, templateIndex, values) {
  const t = theme.sections[S.TEMPLATES][templateIndex];
  let out = '';
  for (let i = t.firstPart; i < t.firstPart + t.partCount; i++) {
    const p = theme.sections[S.PARTS][i];
    out += p.kind === PART.LITERAL ? stringAt(theme, p.a) : formatValue(theme, values[p.a], p.b);
  }
  return clip(out);
}

// ---- pixels -----------------------------------------------------------------
const blend = (src, dst, a) => {
  if (a === 255) return src;
  if (a === 0) return dst;
  const mix = (s, d) => Math.floor((s * a + d * (255 - a) + 127) / 255);
  const r = mix(src >> 11, dst >> 11), g = mix((src >> 5) & 63, (dst >> 5) & 63), b = mix(src & 31, dst & 31);
  return (r << 11) | (g << 5) | b;
};

function fill(fb, x0, y0, w, h, color, inside) {
  for (let y = Math.max(0, y0); y < Math.min(HEIGHT, y0 + h); y++) {
    for (let x = Math.max(0, x0); x < Math.min(WIDTH, x0 + w); x++) if (inside(x, y)) fb[y * WIDTH + x] = color;
  }
}
// Doubled coordinates: pixel centers sit at odd values, so no fractions.
const inCircle = (px, py, cx2, cy2, d) => (2 * px + 1 - cx2) ** 2 + (2 * py + 1 - cy2) ** 2 <= d * d;

function roundRectTest(x, y, w, h, r) {
  if (!r) return () => true;
  r = Math.min(r, w >> 1, h >> 1);
  return (px, py) => {
    const left = px < x + r, right = px >= x + w - r, top = py < y + r, bottom = py >= y + h - r;
    if (!((left || right) && (top || bottom))) return true;
    const cx2 = 2 * (left ? x + r : x + w - r), cy2 = 2 * (top ? y + r : y + h - r);
    return inCircle(px, py, cx2, cy2, 2 * r);
  };
}

// One image row (frame-relative row `row` of frame `frame`) -> colors + alpha.
export function imageRow(theme, im, frame, row, colors, alpha) {
  const data = theme.blob.subarray(im.offset, im.offset + im.size);
  const v = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const r = frame * im.h + row;
  if (im.encoding === ENC.RGB565) {
    const px = im.w * im.h * im.frames;
    for (let x = 0; x < im.w; x++) {
      colors[x] = v.getUint16((r * im.w + x) * 2, true);
      alpha[x] = im.flags & 1 ? data[px * 2 + r * im.w + x] : 255;
    }
    return;
  }
  const n = v.getUint16(0, true);
  let at = v.getUint32(4 + n * 3 + r * 4, true), out = 0;
  const put = (index) => { colors[out] = v.getUint16(4 + index * 2, true); alpha[out] = data[4 + n * 2 + index]; out++; };
  while (out < im.w) {
    const c = data[at++];
    if (c < 128) for (let i = 0; i <= c; i++) put(data[at++]);
    else { const index = data[at++]; for (let i = 0; i < c - 126; i++) put(index); }
  }
}

function frameOf(theme, op, values, timeMs) {
  const im = theme.sections[S.IMAGES][op.a];
  let frame = op.c;
  if (op.flags & OPF.FRAME_VALUE) {
    const v = values[op.c];
    const t = theme.sections[S.TABLES][op.b];
    frame = typeof v === 'number' ? lookup(theme, op.b, Math.round(v)) : t.fallback;
    if (frame === NONE) frame = 0;
  } else if (op.flags & OPF.ANIMATE) frame = Math.floor((timeMs * op.c) / 1000);
  return frame % im.frames;
}

function drawImage(fb, theme, op, values, timeMs) {
  const im = theme.sections[S.IMAGES][op.a];
  const frame = frameOf(theme, op, values, timeMs);
  const colors = new Uint16Array(im.w), alpha = new Uint8Array(im.w);
  for (let row = 0; row < im.h; row++) {
    const y = op.y + row;
    if (y < 0 || y >= HEIGHT) continue;
    imageRow(theme, im, frame, row, colors, alpha);
    for (let x = 0; x < im.w; x++) {
      const px = op.x + x;
      if (px < 0 || px >= WIDTH) continue;
      fb[y * WIDTH + px] = blend(colors[x], fb[y * WIDTH + px], alpha[x]);
    }
  }
}

// Binary search over the font's codepoint-sorted glyphs; fallback if absent.
export function glyphFor(theme, font, codepoint) {
  const glyphs = theme.sections[S.GLYPHS];
  let lo = font.firstGlyph, hi = font.firstGlyph + font.glyphCount - 1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    const c = glyphs[mid].codepoint;
    if (c === codepoint) return glyphs[mid];
    if (c < codepoint) lo = mid + 1; else hi = mid - 1;
  }
  return glyphs[font.firstGlyph + font.fallback];
}

export function textWidth(theme, font, text) {
  let w = 0;
  for (const ch of text) w += glyphFor(theme, font, ch.codePointAt(0)).advance;
  return w;
}

function drawText(fb, theme, op, values) {
  const font = theme.sections[S.FONTS][op.a];
  const text = resolveText(theme, op.b, values);
  const width = textWidth(theme, font, text);
  const align = op.flags & 3;
  let pen = op.x + (align === 1 ? Math.floor((op.w - width) / 2) : align === 2 ? op.w - width : 0);
  const baseline = op.y + font.ascent;
  const x0 = Math.max(0, op.x), x1 = Math.min(WIDTH, op.x + op.w);
  const y0 = Math.max(0, op.y), y1 = Math.min(HEIGHT, op.y + op.h);
  for (const ch of text) {
    const g = glyphFor(theme, font, ch.codePointAt(0));
    const stride = Math.ceil((g.w * font.bpp) / 8);
    const base = font.offset + g.offset;
    for (let gy = 0; gy < g.h; gy++) {
      const y = baseline + g.y + gy;
      if (y < y0 || y >= y1) continue;
      for (let gx = 0; gx < g.w; gx++) {
        const x = pen + g.x + gx;
        if (x < x0 || x >= x1) continue;
        const bit = gx * font.bpp;
        const byte = theme.blob[base + gy * stride + (bit >> 3)];
        const v = (byte >> (8 - font.bpp - (bit & 7))) & ((1 << font.bpp) - 1);
        const a = font.bpp === 4 ? v * 17 : v ? 255 : 0;
        if (a) fb[y * WIDTH + x] = blend(op.color, fb[y * WIDTH + x], a);
      }
    }
    pen += g.advance;
  }
}

// Draws screen `index` with slot `values` (array: number | string | undefined)
// at animation time `timeMs`. Returns a 240x240 RGB565 frame.
export function renderScreen(theme, index, values = [], timeMs = 0) {
  const fb = new Uint16Array(WIDTH * HEIGHT);
  const screen = theme.sections[S.SCREENS][index];
  fb.fill(screen.background);
  for (let i = screen.firstOp; i < screen.firstOp + screen.opCount; i++) {
    const op = theme.sections[S.OPS][i];
    switch (op.type) {
      case OP.RECT: fill(fb, op.x, op.y, op.w, op.h, op.color, roundRectTest(op.x, op.y, op.w, op.h, op.a)); break;
      case OP.CIRCLE: fill(fb, op.x, op.y, op.w, op.w, op.color, (px, py) => inCircle(px, py, 2 * op.x + op.w, 2 * op.y + op.w, op.w)); break;
      case OP.IMAGE: drawImage(fb, theme, op, values, timeMs); break;
      case OP.TEXT: drawText(fb, theme, op, values); break;
    }
  }
  return fb;
}

export const isStringSlot = (theme, i) => theme.sections[S.SLOTS][i].type === SLOT_TYPE.STRING;
