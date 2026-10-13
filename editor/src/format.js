// format.js — the STV2 binary layout: constants, a writer and a validating
// reader. docs/src/content/docs/features/theme2-format.md is the spec; the
// device's src/features/theme2/Stv2File.cpp implements the same checks.

export const MAGIC = 'STV2';
export const VERSION = 1;
export const WIDTH = 240;
export const HEIGHT = 240;
export const NONE = 0xffff;
export const HEADER_SIZE = 32;
export const SECTION_ENTRY = 12;

export const S = {
  STRINGS: 1, SCREENS: 2, OPS: 3, TEMPLATES: 4, PARTS: 5, SLOTS: 6, FORMATS: 7,
  TABLES: 8, ENTRIES: 9, SOURCES: 10, FIELDS: 11, IMAGES: 12, FONTS: 13, GLYPHS: 14, BLOB: 15,
};
export const RECORD_SIZE = {
  1: 8, 2: 8, 3: 20, 4: 4, 5: 6, 6: 8, 7: 12, 8: 8, 9: 8, 10: 12, 11: 36, 12: 16, 13: 16, 14: 12, 15: 0,
};
export const OP = { RECT: 1, CIRCLE: 2, IMAGE: 3, TEXT: 4 };
export const OPF = { FRAME_VALUE: 1, ANIMATE: 2 };
export const PART = { LITERAL: 0, VALUE: 1 };
export const SLOT = { CLOCK: 0, CITY: 1, DATA: 2 };
export const SLOT_TYPE = { NUMBER: 0, STRING: 1 };
export const CLOCK = { H24: 0, H12: 1, MIN: 2, SEC: 3, DAY: 4, MONTH: 5, YEAR: 6, WEEKDAY: 7, AMPM: 8 };
export const ENC = { RGB565: 0, PAL_RLE: 1 };
export const MAX_PATH = 8;

export const LIMITS = {
  file: 900 * 1024, screens: 16, ops: 512, opsPerScreen: 96, slots: 64, pool: 1024,
  sources: 4, fields: 64, images: 64, imageSide: 240, frames: 256, fonts: 8, glyphs: 2048,
  glyphSide: 120, text: 96, strings: 1024, parts: 1024, templates: 1024, tables: 32,
  formats: 64, entries: 1024,
};
// Per section: the most records it may hold.
const MAX_RECORDS = {
  1: LIMITS.strings, 2: LIMITS.screens, 3: LIMITS.ops, 4: LIMITS.templates, 5: LIMITS.parts,
  6: LIMITS.slots, 7: LIMITS.formats, 8: LIMITS.tables, 9: LIMITS.entries, 10: LIMITS.sources,
  11: LIMITS.fields, 12: LIMITS.images, 13: LIMITS.fonts, 14: LIMITS.glyphs,
};

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
export function crc32(bytes, start = 0, end = bytes.length) {
  let c = 0xffffffff;
  for (let i = start; i < end; i++) c = CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
// FNV-1a over the key's UTF-8 bytes, top bit cleared: the path token of an
// object key (the top bit marks array indexes).
export function keyToken(key) {
  let h = 0x811c9dc5;
  for (const b of new TextEncoder().encode(key)) {
    h ^= b;
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return (h & 0x7fffffff) >>> 0;
}
export const indexToken = (i) => (0x80000000 | i) >>> 0;

export function rgb565(hex) {
  const m = /^#?([0-9a-f]{6})$/i.exec(String(hex));
  if (!m) throw new Error(`color: expected #rrggbb, got ${hex}`);
  const v = parseInt(m[1], 16);
  return (((v >> 19) & 0x1f) << 11) | (((v >> 10) & 0x3f) << 5) | ((v >> 3) & 0x1f);
}

// ---- writer -----------------------------------------------------------------
// Each section is an array of plain records; the writer lays them out with the
// spec's field order. The BLOB is a growing byte buffer records point into.
const FIELDS_OF = {
  1: [['u32', 'offset'], ['u16', 'length'], ['u16', null]],
  2: [['u16', 'firstOp'], ['u16', 'opCount'], ['u16', 'seconds'], ['u16', 'background']],
  3: [['u8', 'type'], ['u8', 'flags'], ['i16', 'x'], ['i16', 'y'], ['i16', 'w'], ['i16', 'h'],
      ['u16', 'color'], ['u16', 'color2'], ['u16', 'a'], ['u16', 'b'], ['u16', 'c']],
  4: [['u16', 'firstPart'], ['u16', 'partCount']],
  5: [['u8', 'kind'], ['u8', null], ['u16', 'a'], ['u16', 'b']],
  6: [['u8', 'kind'], ['u8', 'type'], ['u16', 'arg'], ['u16', 'poolOffset'], ['u16', 'poolSize']],
  7: [['i16', 'add'], ['u8', 'decimals'], ['u8', 'width'], ['u8', 'mod'], ['u8', null], ['u16', 'table'], ['u32', null]],
  8: [['u16', 'firstEntry'], ['u16', 'count'], ['u16', 'fallback'], ['u16', null]],
  9: [['i16', 'from'], ['i16', 'to'], ['u16', 'value'], ['u16', null]],
  10: [['u16', 'url'], ['u16', 'seconds'], ['u16', 'firstField'], ['u16', 'fieldCount'], ['u32', null]],
  11: [['u16', 'slot'], ['u8', 'depth'], ['u8', null], ['path', 'path']],
  12: [['u16', 'w'], ['u16', 'h'], ['u16', 'frames'], ['u8', 'encoding'], ['u8', 'flags'], ['u32', 'offset'], ['u32', 'size']],
  13: [['u16', 'firstGlyph'], ['u16', 'glyphCount'], ['u8', 'bpp'], ['u8', 'lineHeight'], ['u8', 'ascent'], ['u8', null],
       ['u16', 'fallback'], ['u16', null], ['u32', 'offset']],
  14: [['u16', 'codepoint'], ['u8', 'w'], ['u8', 'h'], ['i8', 'x'], ['i8', 'y'], ['u8', 'advance'], ['u8', null], ['u32', 'offset']],
};
const WIDTH_OF = { u8: 1, i8: 1, u16: 2, i16: 2, u32: 4, path: 32 };

function writeRecord(view, at, type, rec) {
  for (const [kind, name] of FIELDS_OF[type]) {
    const v = name ? rec[name] ?? 0 : 0;
    switch (kind) {
      case 'u8': view.setUint8(at, v); break;
      case 'i8': view.setInt8(at, v); break;
      case 'u16': view.setUint16(at, v, true); break;
      case 'i16': view.setInt16(at, v, true); break;
      case 'u32': view.setUint32(at, v >>> 0, true); break;
      case 'path': for (let i = 0; i < MAX_PATH; i++) view.setUint32(at + i * 4, (v[i] ?? 0) >>> 0, true); break;
    }
    at += WIDTH_OF[kind];
  }
}
function readRecord(view, at, type) {
  const rec = {};
  for (const [kind, name] of FIELDS_OF[type]) {
    let v;
    switch (kind) {
      case 'u8': v = view.getUint8(at); break;
      case 'i8': v = view.getInt8(at); break;
      case 'u16': v = view.getUint16(at, true); break;
      case 'i16': v = view.getInt16(at, true); break;
      case 'u32': v = view.getUint32(at, true); break;
      case 'path': v = []; for (let i = 0; i < MAX_PATH; i++) v.push(view.getUint32(at + i * 4, true)); break;
    }
    if (name) rec[name] = v;
    at += WIDTH_OF[kind];
  }
  return rec;
}

// sections: { [type]: record[] }, blob: Uint8Array, nameString: index.
export function writeStv2({ sections, blob, name = NONE }) {
  const types = Object.keys(sections).map(Number).filter((t) => sections[t].length).sort((a, b) => a - b);
  if (blob.length) types.push(S.BLOB);
  let at = HEADER_SIZE + types.length * SECTION_ENTRY;
  const layout = [];
  for (const t of types) {
    at = (at + 3) & ~3;
    const size = t === S.BLOB ? blob.length : sections[t].length * RECORD_SIZE[t];
    layout.push({ type: t, offset: at, size });
    at += size;
  }
  const total = (at + 3) & ~3;
  const bytes = new Uint8Array(total);
  const view = new DataView(bytes.buffer);
  bytes.set(new TextEncoder().encode(MAGIC), 0);
  view.setUint16(4, VERSION, true);
  view.setUint16(6, types.length, true);
  view.setUint32(8, total, true);
  view.setUint16(16, WIDTH, true);
  view.setUint16(18, HEIGHT, true);
  view.setUint16(20, name, true);
  layout.forEach((s, i) => {
    const e = HEADER_SIZE + i * SECTION_ENTRY;
    view.setUint16(e, s.type, true);
    view.setUint16(e + 2, RECORD_SIZE[s.type], true);
    view.setUint32(e + 4, s.offset, true);
    view.setUint32(e + 8, s.size, true);
    if (s.type === S.BLOB) bytes.set(blob, s.offset);
    else sections[s.type].forEach((rec, r) => writeRecord(view, s.offset + r * RECORD_SIZE[s.type], s.type, rec));
  });
  view.setUint32(12, crc32(bytes, 16), true);
  return bytes;
}

// ---- reader -----------------------------------------------------------------
export class Stv2Error extends Error {}
const fail = (msg) => { throw new Stv2Error(msg); };

// Parses and fully validates a file: structure, every cross-reference, every
// BLOB range and every image/glyph payload. Anything it accepts the renderer
// can draw without a further bounds check. Returns { name, sections, blob }.
export function readStv2(bytes) {
  if (!(bytes instanceof Uint8Array)) bytes = new Uint8Array(bytes);
  if (bytes.length < HEADER_SIZE) fail('file too short');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (new TextDecoder().decode(bytes.subarray(0, 4)) !== MAGIC) fail('not an STV2 file');
  if (view.getUint16(4, true) !== VERSION) fail(`unsupported format version ${view.getUint16(4, true)}`);
  const count = view.getUint16(6, true);
  const size = view.getUint32(8, true);
  if (size !== bytes.length) fail('file size does not match header');
  if (size > LIMITS.file) fail('file too large');
  if (view.getUint32(12, true) !== crc32(bytes, 16)) fail('checksum mismatch');
  if (view.getUint16(16, true) !== WIDTH || view.getUint16(18, true) !== HEIGHT) fail('unsupported canvas size');
  if (HEADER_SIZE + count * SECTION_ENTRY > size) fail('section table out of range');

  const sections = {};
  let blob = new Uint8Array(0);
  for (let i = 0; i < count; i++) {
    const e = HEADER_SIZE + i * SECTION_ENTRY;
    const type = view.getUint16(e, true), rs = view.getUint16(e + 2, true);
    const offset = view.getUint32(e + 4, true), len = view.getUint32(e + 8, true);
    if (!(type in RECORD_SIZE)) fail(`unknown section ${type}`);
    if (type === S.BLOB ? 'blob' in sections : type in sections) fail(`duplicate section ${type}`);
    if (rs !== RECORD_SIZE[type]) fail(`section ${type}: record size ${rs}`);
    if (offset % 4 || offset < HEADER_SIZE + count * SECTION_ENTRY || offset + len > size) fail(`section ${type}: out of range`);
    if (type === S.BLOB) { blob = bytes.subarray(offset, offset + len); sections.blob = true; continue; }
    if (len % rs) fail(`section ${type}: partial record`);
    if (len / rs > MAX_RECORDS[type]) fail(`section ${type}: too many records`);
    sections[type] = [];
    for (let at = offset; at < offset + len; at += rs) sections[type].push(readRecord(view, at, type));
  }
  delete sections.blob;
  for (const t of Object.keys(RECORD_SIZE)) if (Number(t) !== S.BLOB) sections[t] ??= [];
  validate(sections, blob, view.getUint16(20, true));
  return { name: view.getUint16(20, true), sections, blob };
}

function inBlob(blob, offset, len, what) {
  if (offset + len > blob.length) fail(`${what}: data out of range`);
}
const ref = (index, list, what, optional = false) => {
  if (optional && index === NONE) return;
  if (index >= list.length) fail(`${what}: bad reference ${index}`);
};
const range = (first, n, list, what) => {
  if (first + n > list.length) fail(`${what}: range out of bounds`);
};

function validate(s, blob, name) {
  const strings = s[S.STRINGS], screens = s[S.SCREENS], ops = s[S.OPS];
  ref(name, strings, 'name', true);
  for (const st of strings) inBlob(blob, st.offset, st.length, 'string');
  if (!screens.length) fail('theme has no screens');
  for (const sc of screens) {
    range(sc.firstOp, sc.opCount, ops, 'screen');
    if (sc.opCount > LIMITS.opsPerScreen) fail('screen: too many ops');
  }
  for (const t of s[S.TEMPLATES]) range(t.firstPart, t.partCount, s[S.PARTS], 'template');
  for (const p of s[S.PARTS]) {
    if (p.kind === PART.LITERAL) ref(p.a, strings, 'literal');
    else if (p.kind === PART.VALUE) { ref(p.a, s[S.SLOTS], 'value'); ref(p.b, s[S.FORMATS], 'format', true); }
    else fail(`part kind ${p.kind}`);
  }
  for (const sl of s[S.SLOTS]) {
    if (sl.kind > SLOT.DATA || sl.type > SLOT_TYPE.STRING) fail('slot: bad kind');
    if (sl.kind === SLOT.CLOCK && sl.arg > CLOCK.AMPM) fail('slot: bad clock field');
    if (sl.type === SLOT_TYPE.STRING && (!sl.poolSize || sl.poolOffset + sl.poolSize > LIMITS.pool)) fail('slot: string pool out of range');
  }
  for (const f of s[S.FORMATS]) ref(f.table, s[S.TABLES], 'format table', true);
  for (const t of s[S.TABLES]) range(t.firstEntry, t.count, s[S.ENTRIES], 'table');
  for (const src of s[S.SOURCES]) {
    ref(src.url, strings, 'source url');
    range(src.firstField, src.fieldCount, s[S.FIELDS], 'source');
    if (!src.seconds) fail('source: interval must be positive');
  }
  for (const f of s[S.FIELDS]) {
    ref(f.slot, s[S.SLOTS], 'field slot');
    if (!f.depth || f.depth > MAX_PATH) fail('field: bad path depth');
  }
  for (const im of s[S.IMAGES]) validateImage(im, blob);
  let glyphs = 0;
  for (const f of s[S.FONTS]) {
    range(f.firstGlyph, f.glyphCount, s[S.GLYPHS], 'font');
    if (!f.glyphCount || (f.bpp !== 1 && f.bpp !== 4)) fail('font: bad glyphs or depth');
    if (f.fallback >= f.glyphCount) fail('font: bad fallback');
    let last = -1;
    for (let g = f.firstGlyph; g < f.firstGlyph + f.glyphCount; g++) {
      const gl = s[S.GLYPHS][g];
      if (gl.codepoint <= last) fail('font: glyphs not sorted');
      last = gl.codepoint;
      if (gl.w > LIMITS.glyphSide || gl.h > LIMITS.glyphSide) fail('glyph too large');
      inBlob(blob, f.offset + gl.offset, gl.h * Math.ceil((gl.w * f.bpp) / 8), 'glyph');
    }
    glyphs += f.glyphCount;
  }
  if (glyphs > LIMITS.glyphs) fail('too many glyphs');
  for (const op of ops) {
    switch (op.type) {
      case OP.RECT: case OP.CIRCLE: break;
      case OP.IMAGE: {
        ref(op.a, s[S.IMAGES], 'image op');
        const im = s[S.IMAGES][op.a];
        if (op.w !== im.w || op.h !== im.h) fail('image op: size does not match image');
        if (op.flags & OPF.FRAME_VALUE) { ref(op.b, s[S.TABLES], 'image frame table'); ref(op.c, s[S.SLOTS], 'image frame slot'); }
        else if (!(op.flags & OPF.ANIMATE) && op.c >= im.frames) fail('image op: bad frame');
        break;
      }
      case OP.TEXT: ref(op.a, s[S.FONTS], 'text font'); ref(op.b, s[S.TEMPLATES], 'text template'); break;
      default: fail(`op type ${op.type}`);
    }
  }
}

function validateImage(im, blob) {
  if (!im.w || !im.h || im.w > LIMITS.imageSide || im.h > LIMITS.imageSide) fail('image: bad size');
  if (!im.frames || im.frames > LIMITS.frames) fail('image: bad frame count');
  inBlob(blob, im.offset, im.size, 'image');
  const rows = im.h * im.frames, px = im.w * rows;
  if (im.encoding === ENC.RGB565) {
    if (im.size !== px * 2 + (im.flags & 1 ? px : 0)) fail('image: size does not match pixels');
    return;
  }
  if (im.encoding !== ENC.PAL_RLE) fail('image: unknown encoding');
  const v = new DataView(blob.buffer, blob.byteOffset + im.offset, im.size);
  if (im.size < 4) fail('image: truncated');
  const n = v.getUint16(0, true);
  if (!n || n > 256) fail('image: bad palette');
  const table = 4 + n * 3;
  if (table + rows * 4 > im.size) fail('image: truncated');
  // Every row must decode to exactly w indices below n without overrunning
  // the image -- this is the only pass that touches the RLE stream, so the
  // renderer can then trust it.
  for (let r = 0; r < rows; r++) {
    let at = v.getUint32(table + r * 4, true), out = 0;
    while (out < im.w) {
      if (at >= im.size) fail('image: row out of range');
      const c = v.getUint8(at++);
      const run = c < 128 ? c + 1 : c - 126;
      if (out + run > im.w) fail('image: row overruns width');
      const literal = c < 128 ? run : 1;
      if (at + literal > im.size) fail('image: row out of range');
      for (let i = 0; i < literal; i++) if (v.getUint8(at + i) >= n) fail('image: index outside palette');
      at += literal;
      out += run;
    }
  }
}
