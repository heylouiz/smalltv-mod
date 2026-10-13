// compile.js — theme source (the editor's JSON) + rasterized assets -> STV2.
//
// Source shape (all IDs are [A-Za-z0-9_-]+):
//   { name, background: '#rrggbb',
//     fonts:   { id: { family, size, bpp: 1|4, url? } },        // rasterized by the browser
//     images:  { id: { frames?, encoding?: 'auto'|'palette'|'raw' } },
//     tables:  { id: { entries: [{ from, to?, value }], default? } },
//     formats: { id: { add?, decimals?, width?, mod?, table? } },
//     sources: { id: { url, interval, fields: { name: 'a.b[0]' | 'a.b[0..6]' | { path, type: 'string', size? } } } },
//     screens: [{ seconds?, background?, layers: [layer] }] }
//   layer: { type: 'rect', x, y, w, h, color, radius? }
//        | { type: 'circle', x, y, size, color }
//        | { type: 'image', x, y, image, frame?: n | { value: 'src.field', table }, fps? }
//        | { type: 'text', x, y, w, h?, font, color, align?: 'left'|'center'|'right', text: 'template' }
// Templates: literal text with {TOKEN} or {TOKEN|format}; TOKEN is a clock
// token (HH, MM, WD, ...), CITY, or source.field. '{{' and '}}' are braces.
// Assets: { fonts: { id: { lineHeight, ascent, glyphs: [{ codepoint, w, h, x, y, advance, coverage }] } },
//           images: { id: { width, height, data: RGBA } } }
import {
  S, OP, OPF, PART, SLOT, SLOT_TYPE, NONE, LIMITS, MAX_PATH, keyToken, indexToken, rgb565, writeStv2,
} from './format.js';
import { TABLES, FORMATS, CLOCK_TOKENS } from './builtins.js';
import { encodeImage } from './images.js';

export class CompileError extends Error {}
const fail = (where, msg) => { throw new CompileError(`${where}: ${msg}`); };
const ID = /^[A-Za-z0-9_-]+$/;

// '{HH}:{MM}' -> [{ token: 'HH' }, { literal: ':' }, { token: 'MM' }]
export function parseTemplate(text, where = 'text') {
  if (typeof text !== 'string' || !text.length) fail(where, 'text is empty');
  const parts = [];
  let literal = '';
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch === '{' && text[i + 1] === '{') { literal += '{'; i++; continue; }
    if (ch === '}' && text[i + 1] === '}') { literal += '}'; i++; continue; }
    if (ch === '}') fail(where, "unmatched '}' (write '}}' for a brace)");
    if (ch !== '{') { literal += ch; continue; }
    const end = text.indexOf('}', i);
    if (end < 0) fail(where, "unclosed '{'");
    const [token, format, extra] = text.slice(i + 1, end).split('|').map((s) => s.trim());
    if (!token || extra !== undefined || (format !== undefined && !format)) fail(where, `bad placeholder {${text.slice(i + 1, end)}}`);
    if (literal) parts.push({ literal });
    literal = '';
    parts.push({ token, format });
    i = end;
  }
  if (literal) parts.push({ literal });
  return parts;
}

// 'daily.temperature_2m_max[0..6]' -> [{ suffix: '0', tokens }, ... { suffix: '6', tokens }]
export function parsePath(path, where = 'path') {
  if (typeof path !== 'string' || !path) fail(where, 'empty path');
  let variants = [{ suffix: '', tokens: [] }];
  // Split on dots outside brackets, so 'a[0..6]' stays one segment.
  const segments = [];
  let depth = 0, cur = '';
  for (const ch of path) {
    if (ch === '[') depth++;
    if (ch === ']') depth--;
    if (ch === '.' && !depth) { segments.push(cur); cur = ''; } else cur += ch;
  }
  segments.push(cur);
  for (const seg of segments) {
    const m = /^([^[\]]*)((?:\[\d+(?:\.\.\d+)?\])*)$/.exec(seg);
    if (!m || (!m[1] && !m[2])) fail(where, `bad segment '${seg}'`);
    if (m[1]) variants.forEach((v) => v.tokens.push(keyToken(m[1])));
    for (const idx of m[2].match(/\[[^\]]+\]/g) || []) {
      const [a, b = a] = idx.slice(1, -1).split('..').map(Number);
      if (b < a || b - a > 31) fail(where, `bad range ${idx}`);
      const next = [];
      for (const v of variants) {
        for (let i = a; i <= b; i++) {
          next.push({ suffix: a === b ? v.suffix : v.suffix + i, tokens: [...v.tokens, indexToken(i)] });
        }
      }
      variants = next;
    }
  }
  for (const v of variants) if (v.tokens.length > MAX_PATH) fail(where, `deeper than ${MAX_PATH} levels`);
  return variants;
}

// Expands a source's fields: { max: 'daily.x[0..6]' } -> max0 .. max6.
function sourceFields(src, id) {
  const fields = [];
  for (const [name, spec] of Object.entries(src.fields || {})) {
    if (!ID.test(name)) fail(`sources.${id}`, `bad field name '${name}'`);
    const def = typeof spec === 'string' ? { path: spec } : spec;
    const type = def.type === 'string' ? SLOT_TYPE.STRING : SLOT_TYPE.NUMBER;
    for (const v of parsePath(def.path, `sources.${id}.fields.${name}`)) {
      fields.push({ name: name + v.suffix, tokens: v.tokens, type, size: def.size || 24 });
    }
  }
  return fields;
}

const codepoints = (s) => [...s].map((c) => c.codePointAt(0));
const LATIN1 = (() => { const s = new Set(); for (let c = 0x20; c <= 0xff; c++) if (c < 0x7f || c >= 0xa0) s.add(c); return s; })();
const NUMERIC = new Set(codepoints('0123456789-.'));

// What the theme needs, worked out once and shared by compile() and
// requiredGlyphs(): the resolved tables/formats and, for each text layer,
// its parsed template with every token resolved to a slot description.
function analyze(source) {
  const tables = { ...TABLES, ...(source.tables || {}) };
  const formats = { ...FORMATS, ...(source.formats || {}) };
  const sources = source.sources || {};
  const fieldIndex = {};
  for (const [id, src] of Object.entries(sources)) {
    if (!ID.test(id)) fail('sources', `bad id '${id}'`);
    for (const f of sourceFields(src, id)) fieldIndex[`${id}.${f.name}`] = f;
  }
  const resolve = (token, format, where) => {
    let slot, fmt = format;
    if (token in CLOCK_TOKENS) { slot = { key: `clock:${CLOCK_TOKENS[token][0]}`, kind: SLOT.CLOCK, arg: CLOCK_TOKENS[token][0], type: SLOT_TYPE.NUMBER }; fmt ??= CLOCK_TOKENS[token][1]; }
    else if (token === 'CITY') slot = { key: 'city', kind: SLOT.CITY, arg: 0, type: SLOT_TYPE.STRING, size: 32 };
    else if (token in fieldIndex) { const f = fieldIndex[token]; slot = { key: `data:${token}`, kind: SLOT.DATA, type: f.type, size: f.size }; fmt ??= f.type === SLOT_TYPE.NUMBER ? 'int' : undefined; }
    else fail(where, `unknown placeholder {${token}}`);
    if (fmt !== undefined && !(fmt in formats)) fail(where, `unknown format '${fmt}'`);
    if (fmt !== undefined && formats[fmt].table && !(formats[fmt].table in tables)) fail(where, `format '${fmt}' uses unknown table '${formats[fmt].table}'`);
    return { slot, format: fmt };
  };
  if (!Array.isArray(source.screens) || !source.screens.length) fail('screens', 'a theme needs at least one screen');
  const screens = source.screens.map((sc, si) => ({
    ...sc,
    layers: (sc.layers || []).map((layer, li) => {
      const where = `screens[${si}].layers[${li}]`;
      if (layer.type === 'text') {
        return { ...layer, where, parts: parseTemplate(layer.text, where).map((p) => (p.literal !== undefined ? p : { ...p, ...resolve(p.token, p.format, where) })) };
      }
      if (layer.type === 'image' && layer.frame && typeof layer.frame === 'object') {
        if (!(layer.frame.table in tables)) fail(where, `unknown table '${layer.frame.table}'`);
        return { ...layer, where, frameSlot: resolve(layer.frame.value, undefined, where).slot };
      }
      return { ...layer, where };
    }),
  }));
  return { tables, formats, sources, screens };
}

// Codepoints each font must contain: every literal it draws, the table
// strings its formats can produce, digits for numbers and Latin-1 for free
// text (city, string fields). The browser rasterizes exactly this set.
export function requiredGlyphs(source) {
  const { tables, formats, screens } = analyze(source);
  const need = {};
  for (const sc of screens) for (const layer of sc.layers) {
    if (layer.type !== 'text') continue;
    const set = (need[layer.font] ??= new Set(codepoints('-')));
    for (const p of layer.parts) {
      if (p.literal !== undefined) { codepoints(p.literal).forEach((c) => set.add(c)); continue; }
      if (p.slot.type === SLOT_TYPE.STRING) { LATIN1.forEach((c) => set.add(c)); continue; }
      const table = p.format && formats[p.format].table;
      if (table) {
        for (const e of tables[table].entries) codepoints(String(e.value)).forEach((c) => set.add(c));
        if (tables[table].default !== undefined) codepoints(String(tables[table].default)).forEach((c) => set.add(c));
      } else NUMERIC.forEach((c) => set.add(c));
    }
  }
  return need;
}

export function compileTheme(source, assets = {}) {
  const { tables, formats, sources, screens } = analyze(source);
  const sec = Object.fromEntries(Object.values(S).filter((t) => t !== S.BLOB).map((t) => [t, []]));
  const blob = [];
  let blobLength = 0;
  const appendBlob = (bytes) => { const at = blobLength; blob.push(bytes); blobLength += bytes.length; return at; };

  const strings = new Map();
  const string = (text) => {
    text = String(text);
    if (!strings.has(text)) {
      const bytes = new TextEncoder().encode(text);
      strings.set(text, sec[S.STRINGS].length);
      sec[S.STRINGS].push({ offset: appendBlob(bytes), length: bytes.length });
    }
    return strings.get(text);
  };
  // Memoizes a record builder by key: the first use appends, later uses
  // return the same index. `.find(key)` looks up without creating.
  const ids = () => {
    const m = new Map();
    const get = (key, make) => { if (!m.has(key)) m.set(key, make()); return m.get(key); };
    get.find = (key) => m.get(key);
    return get;
  };

  // Tables hold strings for text and plain numbers for image frames.
  const tableIndex = ids();
  const table = (id) => tableIndex(id, () => {
    const t = tables[id];
    const first = sec[S.ENTRIES].length;
    const value = (v) => (typeof v === 'number' ? v : string(v));
    for (const e of t.entries) sec[S.ENTRIES].push({ from: e.from, to: e.to ?? e.from, value: value(e.value) });
    sec[S.TABLES].push({ firstEntry: first, count: t.entries.length, fallback: t.default === undefined ? NONE : value(t.default) });
    return sec[S.TABLES].length - 1;
  });
  const formatIndex = ids();
  const format = (id) => (id === undefined ? NONE : formatIndex(id, () => {
    const f = formats[id];
    sec[S.FORMATS].push({ add: f.add || 0, decimals: f.decimals || 0, width: f.width || 0, mod: f.mod || 0, table: f.table ? table(f.table) : NONE });
    return sec[S.FORMATS].length - 1;
  }));
  let pool = 0;
  const slotIndex = ids();
  const slot = (desc) => slotIndex(desc.key, () => {
    const rec = { kind: desc.kind, type: desc.type, arg: desc.arg || 0, poolOffset: 0, poolSize: 0 };
    if (desc.type === SLOT_TYPE.STRING) {
      rec.poolOffset = pool;
      rec.poolSize = desc.size;
      pool += desc.size;
      if (pool > LIMITS.pool) fail('values', `string values need more than ${LIMITS.pool} bytes`);
    }
    sec[S.SLOTS].push(rec);
    return sec[S.SLOTS].length - 1;
  });

  // Fonts: pack each glyph's 0-255 coverage at the font's depth.
  const fontIndex = ids();
  const font = (id, where) => fontIndex(id, () => {
    const def = (source.fonts || {})[id], raster = (assets.fonts || {})[id];
    if (!def) fail(where, `unknown font '${id}'`);
    if (!raster) fail(where, `font '${id}' was not rasterized`);
    const bpp = def.bpp === 1 ? 1 : 4;
    const glyphs = [...raster.glyphs].sort((a, b) => a.codepoint - b.codepoint);
    const first = sec[S.GLYPHS].length;
    const chunks = [];
    let size = 0;
    for (const g of glyphs) {
      const stride = Math.ceil((g.w * bpp) / 8);
      const bits = new Uint8Array(stride * g.h);
      for (let y = 0; y < g.h; y++) for (let x = 0; x < g.w; x++) {
        const cov = g.coverage[y * g.w + x];
        const v = bpp === 4 ? Math.round(cov / 17) : cov >= 128 ? 1 : 0;
        const bit = x * bpp;
        bits[y * stride + (bit >> 3)] |= v << (8 - bpp - (bit & 7));
      }
      sec[S.GLYPHS].push({ codepoint: g.codepoint, w: g.w, h: g.h, x: g.x, y: g.y, advance: g.advance, offset: size });
      chunks.push(bits);
      size += bits.length;
    }
    const all = new Uint8Array(size);
    let at = 0;
    for (const c of chunks) { all.set(c, at); at += c.length; }
    const fallback = Math.max(0, glyphs.findIndex((g) => g.codepoint === 0x3f)); // '?'
    sec[S.FONTS].push({ firstGlyph: first, glyphCount: glyphs.length, bpp, lineHeight: raster.lineHeight, ascent: raster.ascent, fallback, offset: appendBlob(all) });
    return sec[S.FONTS].length - 1;
  });

  const imageIndex = ids();
  const image = (id, where) => imageIndex(id, () => {
    const def = (source.images || {})[id], pixels = (assets.images || {})[id];
    if (!def) fail(where, `unknown image '${id}'`);
    if (!pixels) fail(where, `image '${id}' has no pixels`);
    const enc = encodeImage(pixels, { frames: def.frames || 1, mode: def.encoding || 'auto' });
    sec[S.IMAGES].push({ w: enc.w, h: enc.h, frames: enc.frames, encoding: enc.encoding, flags: enc.flags, offset: appendBlob(enc.data), size: enc.data.length });
    return sec[S.IMAGES].length - 1;
  });

  const templateIndex = ids();
  const template = (layer) => templateIndex(JSON.stringify(layer.parts), () => {
    const first = sec[S.PARTS].length;
    for (const p of layer.parts) {
      if (p.literal !== undefined) sec[S.PARTS].push({ kind: PART.LITERAL, a: string(p.literal), b: 0 });
      else sec[S.PARTS].push({ kind: PART.VALUE, a: slot(p.slot), b: format(p.format) });
    }
    sec[S.TEMPLATES].push({ firstPart: first, partCount: layer.parts.length });
    return sec[S.TEMPLATES].length - 1;
  });

  const background = rgb565(source.background || '#000000');
  const int = (v, where, name, lo = -240, hi = 480) => {
    if (!Number.isInteger(v) || v < lo || v > hi) fail(where, `${name} must be an integer from ${lo} to ${hi}`);
    return v;
  };
  for (const sc of screens) {
    const firstOp = sec[S.OPS].length;
    for (const layer of sc.layers) {
      const { where } = layer;
      const op = { type: 0, flags: 0, x: int(layer.x ?? 0, where, 'x'), y: int(layer.y ?? 0, where, 'y'), w: 0, h: 0, color: 0, color2: 0, a: 0, b: 0, c: 0 };
      switch (layer.type) {
        case 'rect':
          Object.assign(op, { type: OP.RECT, w: int(layer.w, where, 'w', 1, 240), h: int(layer.h, where, 'h', 1, 240), color: rgb565(layer.color), a: int(layer.radius ?? 0, where, 'radius', 0, 120) });
          break;
        case 'circle': {
          const size = int(layer.size, where, 'size', 1, 240);
          Object.assign(op, { type: OP.CIRCLE, w: size, h: size, color: rgb565(layer.color) });
          break;
        }
        case 'image': {
          const index = image(layer.image, where);
          const im = sec[S.IMAGES][index];
          Object.assign(op, { type: OP.IMAGE, w: im.w, h: im.h, a: index });
          if (layer.frame && typeof layer.frame === 'object') Object.assign(op, { flags: OPF.FRAME_VALUE, b: table(layer.frame.table), c: slot(layer.frameSlot) });
          else if (layer.fps) Object.assign(op, { flags: OPF.ANIMATE, c: int(layer.fps, where, 'fps', 1, 30) });
          else op.c = int(layer.frame ?? 0, where, 'frame', 0, im.frames - 1);
          break;
        }
        case 'text': {
          const index = font(layer.font, where);
          const f = sec[S.FONTS][index];
          const align = { left: 0, center: 1, right: 2 }[layer.align || 'left'];
          if (align === undefined) fail(where, `bad align '${layer.align}'`);
          Object.assign(op, { type: OP.TEXT, flags: align, w: int(layer.w ?? 240, where, 'w', 1, 240), h: int(layer.h ?? f.lineHeight, where, 'h', 1, 240), color: rgb565(layer.color || '#ffffff'), a: index, b: template(layer) });
          break;
        }
        default: fail(where, `unknown layer type '${layer.type}'`);
      }
      sec[S.OPS].push(op);
    }
    const opCount = sec[S.OPS].length - firstOp;
    if (opCount > LIMITS.opsPerScreen) fail('screens', `a screen holds at most ${LIMITS.opsPerScreen} layers`);
    sec[S.SCREENS].push({ firstOp, opCount, seconds: screens.length > 1 ? int(sc.seconds ?? 10, 'screens', 'seconds', 1, 3600) : 0, background: sc.background ? rgb565(sc.background) : background });
  }

  // Sources last: only fields some layer reads get a slot; the rest are not fetched.
  for (const [id, src] of Object.entries(sources)) {
    const firstField = sec[S.FIELDS].length;
    for (const f of sourceFields(src, id)) {
      const used = slotIndex.find(`data:${id}.${f.name}`);
      if (used === undefined) continue;
      sec[S.FIELDS].push({ slot: used, depth: f.tokens.length, path: f.tokens });
    }
    if (!/^http:\/\/[^/?#]+/.test(src.url || '')) fail(`sources.${id}`, 'url must start with http:// (the device cannot afford TLS)');
    if (sec[S.FIELDS].length > firstField) {
      sec[S.SOURCES].push({ url: string(src.url), seconds: int(src.interval ?? 900, `sources.${id}`, 'interval', 60, 86400), firstField, fieldCount: sec[S.FIELDS].length - firstField });
    }
  }

  const name = string(source.name || 'Theme');
  const bytes = new Uint8Array(blobLength);
  let at = 0;
  for (const b of blob) { bytes.set(b, at); at += b.length; }
  for (const [type, max] of [[S.STRINGS, LIMITS.strings], [S.PARTS, LIMITS.parts], [S.TEMPLATES, LIMITS.templates], [S.OPS, LIMITS.ops],
    [S.SLOTS, LIMITS.slots], [S.TABLES, LIMITS.tables], [S.FORMATS, LIMITS.formats], [S.ENTRIES, LIMITS.entries], [S.SOURCES, LIMITS.sources],
    [S.FIELDS, LIMITS.fields], [S.IMAGES, LIMITS.images], [S.FONTS, LIMITS.fonts], [S.GLYPHS, LIMITS.glyphs], [S.SCREENS, LIMITS.screens]]) {
    if (sec[type].length > max) fail('theme', `too many records in section ${type} (${sec[type].length} > ${max})`);
  }
  const out = writeStv2({ sections: sec, blob: bytes, name });
  if (out.length > LIMITS.file) fail('theme', `compiled theme is ${out.length} bytes, over ${LIMITS.file}`);
  return out;
}
