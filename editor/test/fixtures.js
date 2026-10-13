// Deterministic fonts and images for tests: no canvas under Node, so glyphs
// and pixels are generated from simple rules.

// A font where every glyph is a w x h block whose coverage ramps left to
// right, with the codepoint's low bits punched out so glyphs differ.
export function blockFont(codepoints, { w = 6, h = 8, ascent = 7, advance = 7, lineHeight = 10 } = {}) {
  const glyphs = [...new Set(codepoints)].map((codepoint) => {
    const coverage = new Uint8Array(w * h);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      coverage[y * w + x] = (codepoint >> (y % 5)) & 1 && x === w - 1 ? 0 : Math.round((255 * (x + 1)) / w);
    }
    return { codepoint, w, h, x: 0, y: -ascent, advance, coverage };
  });
  return { lineHeight, ascent, glyphs };
}

export function solidImage(width, height, [r, g, b, a = 255]) {
  const data = new Uint8Array(width * height * 4);
  for (let i = 0; i < width * height; i++) data.set([r, g, b, a], i * 4);
  return { width, height, data };
}

// Horizontal stripes of `colors`, one per frame row band: useful for frames.
export function stripes(width, height, colors) {
  const data = new Uint8Array(width * height * 4);
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) data.set(colors[y % colors.length], (y * width + x) * 4);
  return { width, height, data };
}

// Pseudo-random noise with many colors (forces quantization).
export function noise(width, height, seed = 1) {
  const data = new Uint8Array(width * height * 4);
  let s = seed;
  for (let i = 0; i < data.length; i++) {
    s = (Math.imul(s, 1103515245) + 12345) >>> 0;
    data[i] = i % 4 === 3 ? 255 : s >>> 24;
  }
  return { width, height, data };
}
