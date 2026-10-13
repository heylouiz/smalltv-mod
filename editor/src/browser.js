// browser.js — the parts that need a browser: rasterizing fonts and decoding
// images through a canvas, and a live preview that renders the compiled theme
// with real data. Everything else in the library is plain JS.
import { readStv2, WIDTH, HEIGHT, S } from './format.js';
import { renderScreen } from './render.js';
import { slotValues, applySource, sourceUrl } from './values.js';

const canvas2d = (w, h) => {
  const c = typeof OffscreenCanvas !== 'undefined' ? new OffscreenCanvas(w, h) : Object.assign(document.createElement('canvas'), { width: w, height: h });
  return c.getContext('2d', { willReadFrequently: true });
};

// Loads a Google Fonts family (any weight) so canvas text can use it. The CSS
// API answers with @font-face rules; each rule's woff2 is loaded as a FontFace.
export async function loadGoogleFont(family, weight = 400) {
  const css = await (await fetch(`https://fonts.googleapis.com/css2?family=${encodeURIComponent(family)}:wght@${weight}&display=block`)).text();
  const faces = [...css.matchAll(/@font-face\s*{([^}]*)}/g)].map((m) => {
    const url = /url\(([^)]+)\)/.exec(m[1])?.[1];
    const range = /unicode-range:\s*([^;]+);/.exec(m[1])?.[1];
    return new FontFace(family, `url(${url})`, { weight: String(weight), ...(range ? { unicodeRange: range } : {}) });
  });
  if (!faces.length) throw new Error(`Google Fonts has no family '${family}'`);
  for (const face of faces) document.fonts.add(await face.load());
}

// Loads a font file (File, Blob or ArrayBuffer) under `family`.
export async function loadFontFile(family, data, weight = 400) {
  const buffer = data instanceof ArrayBuffer ? data : await data.arrayBuffer();
  document.fonts.add(await new FontFace(family, buffer, { weight: String(weight) }).load());
}

// Draws each codepoint with the canvas's own anti-aliasing and keeps the
// alpha channel as coverage, cropped to the glyph's ink box. The result is the
// compiler's font asset: { lineHeight, ascent, glyphs }.
export function rasterizeFont({ family, size, weight = 400 }, codepoints) {
  if (size < 6 || size > 110) throw new Error('font size must be 6-110 px');
  const ctx = canvas2d(4 * size, 4 * size);
  const font = `${weight} ${size}px "${family}"`;
  ctx.font = font;
  ctx.textBaseline = 'alphabetic';
  const probe = ctx.measureText('ÁHgjy');
  const ascent = Math.ceil(probe.fontBoundingBoxAscent ?? probe.actualBoundingBoxAscent);
  const descent = Math.ceil(probe.fontBoundingBoxDescent ?? probe.actualBoundingBoxDescent);
  const origin = 2 * size;
  const glyphs = [];
  for (const codepoint of new Set(codepoints)) {
    const ch = String.fromCodePoint(codepoint);
    const m = ctx.measureText(ch);
    const x0 = Math.floor(-m.actualBoundingBoxLeft), x1 = Math.ceil(m.actualBoundingBoxRight);
    const y0 = Math.floor(-m.actualBoundingBoxAscent), y1 = Math.ceil(m.actualBoundingBoxDescent);
    const w = Math.max(0, Math.min(120, x1 - x0)), h = Math.max(0, Math.min(120, y1 - y0));
    const advance = Math.min(255, Math.round(m.width));
    let coverage = new Uint8Array(0);
    if (w && h) {
      ctx.clearRect(0, 0, 4 * size, 4 * size);
      ctx.fillStyle = '#fff';
      ctx.fillText(ch, origin, origin);
      const px = ctx.getImageData(origin + x0, origin + y0, w, h).data;
      coverage = new Uint8Array(w * h);
      for (let i = 0; i < w * h; i++) coverage[i] = px[i * 4 + 3];
    }
    glyphs.push({ codepoint, w: w && h ? w : 0, h: w && h ? h : 0, x: Math.max(-128, x0), y: Math.max(-128, y0), advance, coverage });
  }
  return { lineHeight: Math.min(255, ascent + descent), ascent: Math.min(255, ascent), glyphs, font };
}

// Decodes an image (File, Blob, URL or ImageBitmap) into RGBA pixels at
// `width` x `height` (defaults to its own size, capped to the screen).
export async function loadImagePixels(input, { width, height } = {}) {
  let bitmap = input;
  if (typeof input === 'string') bitmap = await createImageBitmap(await (await fetch(input)).blob());
  else if (input instanceof Blob) bitmap = await createImageBitmap(input);
  const w = width || Math.min(WIDTH, bitmap.width), h = height || Math.min(HEIGHT * 256, bitmap.height);
  const ctx = canvas2d(w, h);
  ctx.drawImage(bitmap, 0, 0, w, h);
  return { width: w, height: h, data: ctx.getImageData(0, 0, w, h).data };
}

// RGB565 frame -> canvas ImageData.
export function toImageData(fb, target) {
  const img = target || new ImageData(WIDTH, HEIGHT);
  for (let i = 0; i < fb.length; i++) {
    const c = fb[i];
    img.data[i * 4] = ((c >> 11) * 527 + 23) >> 6;
    img.data[i * 4 + 1] = (((c >> 5) & 63) * 259 + 33) >> 6;
    img.data[i * 4 + 2] = ((c & 31) * 527 + 23) >> 6;
    img.data[i * 4 + 3] = 255;
  }
  return img;
}

// Live preview on a 240x240 canvas: the clock ticks, screens rotate on their
// own durations, and every source is fetched from the browser (Open-Meteo
// allows it) on its interval, exactly as the device would.
export class Preview {
  constructor(canvas, { location = null, city = '' } = {}) {
    this.ctx = canvas.getContext('2d');
    this.location = location;
    this.city = city;
    this.data = [];
    this.timers = [];
    this.screen = 0;
    this.errors = [];
  }
  load(bytes) {
    this.stop();
    this.theme = readStv2(bytes);
    this.data = [];
    this.screen = 0;
    this.shownAt = performance.now();
    this.started = performance.now();
    this.theme.sections[S.SOURCES].forEach((src, i) => {
      const fetchOne = () => this.fetchSource(i);
      fetchOne();
      this.timers.push(setInterval(fetchOne, src.seconds * 1000));
    });
    // 10 frames per second is plenty for a clock and matches theme animations.
    const tick = () => { this.draw(); this.frame = setTimeout(tick, 100); };
    tick();
    return this.theme;
  }
  async fetchSource(i) {
    const url = sourceUrl(this.theme, i, this.location);
    if (!url) return;
    try {
      // The device fetches over plain HTTP; a page served over HTTPS may not,
      // so the preview upgrades there (fine for public APIs like Open-Meteo).
      const target = globalThis.location?.protocol === 'https:' ? url.replace(/^http:/, 'https:') : url;
      applySource(this.theme, i, await (await fetch(target)).json(), this.data);
    } catch (e) {
      this.errors.push(`${url}: ${e.message}`);
    }
  }
  draw() {
    const screens = this.theme.sections[S.SCREENS];
    const now = performance.now();
    const seconds = screens[this.screen].seconds;
    if (seconds && now - this.shownAt >= seconds * 1000) {
      this.screen = (this.screen + 1) % screens.length;
      this.shownAt = now;
    }
    const values = slotValues(this.theme, { date: new Date(), city: this.city, data: this.data });
    const fb = renderScreen(this.theme, this.screen, values, now - this.started);
    this.ctx.putImageData(toImageData(fb), 0, 0);
  }
  stop() {
    clearTimeout(this.frame);
    this.timers.forEach(clearInterval);
    this.timers = [];
  }
}

// ---- assets -------------------------------------------------------------------
const loadedFonts = new Map();
// Everything compileTheme() needs from the browser: each font loaded (Google
// Fonts by name, or a URL) and rasterized for exactly the glyphs the theme
// uses, and each image decoded (a preset drawn by code, or a URL/data URL).
export async function buildAssets(source, requiredGlyphs, presets = {}) {
  const need = requiredGlyphs(source);
  const fonts = {}, images = {};
  for (const [id, def] of Object.entries(source.fonts || {})) {
    const family = def.google || def.family || id;
    const weight = def.weight || 400;
    const key = `${family}:${weight}:${def.url || ''}`;
    if (!loadedFonts.has(key)) {
      loadedFonts.set(key, def.url
        ? fetch(def.url).then((r) => r.arrayBuffer()).then((b) => loadFontFile(family, b, weight))
        : def.google ? loadGoogleFont(family, weight) : Promise.resolve());
    }
    await loadedFonts.get(key);
    // '?' is the fallback glyph for characters the font was not given.
    fonts[id] = rasterizeFont({ family, size: def.size || 16, weight }, [...(need[id] || []), 0x3f]);
  }
  for (const [id, def] of Object.entries(source.images || {})) {
    let input = def.src;
    if (def.preset) {
      const draw = presets[def.preset];
      if (!draw) throw new Error(`images.${id}: unknown preset '${def.preset}'`);
      const c = def.preset === 'gradient' ? draw(def.from, def.to, def.width, def.height) : draw(def.size);
      input = c.transferToImageBitmap();
    }
    if (!input) throw new Error(`images.${id}: needs a src or a preset`);
    images[id] = await loadImagePixels(input, { width: def.preset ? 0 : def.width, height: def.preset ? 0 : def.height });
  }
  return { fonts, images };
}
