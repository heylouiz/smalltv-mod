// values.js — fills a theme's slots the way the device does: clock fields
// from the local time, the city from Location, data fields from each source's
// JSON response.
import { S, SLOT, SLOT_TYPE, CLOCK, keyToken, indexToken } from './format.js';
import { stringAt } from './render.js';

export function clockField(date, field) {
  switch (field) {
    case CLOCK.H24: return date.getHours();
    case CLOCK.H12: return date.getHours() % 12 || 12;
    case CLOCK.MIN: return date.getMinutes();
    case CLOCK.SEC: return date.getSeconds();
    case CLOCK.DAY: return date.getDate();
    case CLOCK.MONTH: return date.getMonth() + 1;
    case CLOCK.YEAR: return date.getFullYear();
    case CLOCK.WEEKDAY: return date.getDay();
    case CLOCK.AMPM: return date.getHours() < 12 ? 0 : 1;
  }
  return undefined;
}

// Truncates a string to `size` bytes of UTF-8 without splitting a character,
// like the device's fixed-size string pool.
export function fitString(text, size) {
  const bytes = new TextEncoder().encode(text);
  if (bytes.length <= size) return text;
  let end = size;
  while (end > 0 && (bytes[end] & 0xc0) === 0x80) end--;
  return new TextDecoder().decode(bytes.subarray(0, end));
}

// Clock and city slots; `data` (from applySource) supplies the rest.
// `date` null means the clock is not synced yet.
export function slotValues(theme, { date = new Date(), city = '', data = [] } = {}) {
  return theme.sections[S.SLOTS].map((slot, i) => {
    if (slot.kind === SLOT.CLOCK) return date ? clockField(date, slot.arg) : undefined;
    if (slot.kind === SLOT.CITY) return city ? fitString(city, slot.poolSize) : undefined;
    return data[i];
  });
}

// Walks a parsed JSON response and stores each scalar whose path matches one
// of source `index`'s fields. Numbers (and booleans, as 1/0) go to number
// slots, strings to string slots; anything else is ignored. A later match
// overwrites an earlier one, as on the device's streaming parser.
export function applySource(theme, index, json, data = []) {
  const src = theme.sections[S.SOURCES][index];
  const fields = theme.sections[S.FIELDS].slice(src.firstField, src.firstField + src.fieldCount);
  const slots = theme.sections[S.SLOTS];
  const visit = (node, path) => {
    if (path.length > 8) return;
    if (Array.isArray(node)) { node.forEach((v, i) => visit(v, [...path, indexToken(i)])); return; }
    if (node && typeof node === 'object') { for (const [k, v] of Object.entries(node)) visit(v, [...path, keyToken(k)]); return; }
    for (const f of fields) {
      if (f.depth !== path.length || !path.every((t, i) => t === f.path[i])) continue;
      const slot = slots[f.slot];
      if (slot.type === SLOT_TYPE.NUMBER && (typeof node === 'number' || typeof node === 'boolean')) data[f.slot] = Number(node);
      else if (slot.type === SLOT_TYPE.STRING && typeof node === 'string') data[f.slot] = fitString(node, slot.poolSize);
    }
  };
  visit(json, []);
  return data;
}

// The URL a source fetches: {lat}/{lon} filled with four decimals. Returns
// null while no location is set, so the source is skipped, like the device.
export function sourceUrl(theme, index, location) {
  const url = stringAt(theme, theme.sections[S.SOURCES][index].url);
  if (!/\{(lat|lon)\}/.test(url)) return url;
  if (!location || (!location.lat && !location.lon)) return null;
  return url.replaceAll('{lat}', location.lat.toFixed(4)).replaceAll('{lon}', location.lon.toFixed(4));
}
