// ui.js — the editor mounted into the device's Theme tab (or any page):
// the theme as JSON, a live preview with real data, and "Send to device".
// A visual layer editor comes later; the JSON is the same source format.
import { compileTheme, requiredGlyphs } from './compile.js';
import { buildAssets, Preview } from './browser.js';
import { PRESETS, exampleTheme } from './presets.js';
import { S } from './format.js';

const CSS = `
.stv-ed{display:flex;flex-direction:column;gap:10px}
.stv-ed .stv-top{display:flex;gap:12px;flex-wrap:wrap;align-items:flex-start}
.stv-ed canvas{width:240px;height:240px;image-rendering:pixelated;border-radius:8px;border:1px solid var(--bd,#333);background:#000}
.stv-ed .stv-side{flex:1;min-width:180px;display:flex;flex-direction:column;gap:8px;font-size:13px}
.stv-ed textarea{width:100%;min-height:320px;font:12px/1.4 ui-monospace,Menlo,Consolas,monospace;background:#0b0e13;color:var(--fg,#e6edf3);border:1px solid var(--bd,#333);border-radius:8px;padding:8px;resize:vertical}
.stv-ed .stv-row{display:flex;gap:8px;flex-wrap:wrap;align-items:center}
.stv-ed button{background:var(--acc,#3fb950);color:#04130a;border:0;padding:9px 14px;border-radius:9px;font-weight:600;cursor:pointer}
.stv-ed button.sec{background:#222b36;color:var(--fg,#e6edf3)}
.stv-ed button:disabled{opacity:.5}
.stv-ed .stv-msg{color:var(--mut,#8b96a5);white-space:pre-wrap}
.stv-ed .stv-err{color:var(--red,#f85149)}
.stv-ed input[type=text]{width:120px;padding:6px 8px;background:#0b0e13;border:1px solid var(--bd,#333);border-radius:8px;color:var(--fg,#e6edf3)}
@media (max-width:520px){.stv-ed canvas{width:100%;height:auto;aspect-ratio:1}}
`;

const el = (tag, props = {}, ...children) => {
  const e = Object.assign(document.createElement(tag), props);
  e.append(...children);
  return e;
};

// options: { api: '' (same origin) | null (no device), location: {lat, lon}, city, onInstalled }
export function mount(container, options = {}) {
  const api = options.api === undefined ? '' : options.api;
  if (!document.getElementById('stv-ed-css')) document.head.append(el('style', { id: 'stv-ed-css', textContent: CSS }));
  const canvas = el('canvas', { width: 240, height: 240 });
  const text = el('textarea', { spellcheck: false });
  const msg = el('div', { className: 'stv-msg' });
  const screenInfo = el('span', { className: 'stv-msg' });
  const btnPreview = el('button', { textContent: 'Preview' });
  const btnSend = el('button', { textContent: 'Send to device', disabled: api === null });
  const btnNext = el('button', { className: 'sec', textContent: 'Next screen' });
  const btnReset = el('button', { className: 'sec', textContent: 'Start from the example' });
  const imageId = el('input', { type: 'text', placeholder: 'image id' });
  const imageFile = el('input', { type: 'file', accept: 'image/*' });
  const btnImage = el('button', { className: 'sec', textContent: 'Add image' });
  container.textContent = '';
  container.append(el('div', { className: 'stv-ed' },
    el('div', { className: 'stv-top' }, canvas,
      el('div', { className: 'stv-side' },
        el('div', { className: 'stv-row' }, btnPreview, btnSend),
        el('div', { className: 'stv-row' }, btnNext, screenInfo),
        msg)),
    text,
    el('div', { className: 'stv-row' }, imageId, imageFile, btnImage, btnReset)));

  const preview = new Preview(canvas, { location: options.location, city: options.city });
  let bytes = null;
  const say = (m, error = false) => { msg.textContent = m; msg.className = error ? 'stv-msg stv-err' : 'stv-msg'; };
  const parse = () => JSON.parse(text.value);

  async function compile() {
    const source = parse();
    const assets = await buildAssets(source, requiredGlyphs, PRESETS);
    bytes = compileTheme(source, assets);
    const theme = preview.load(bytes);
    const glyphs = theme.sections[S.GLYPHS].length;
    say(`${(bytes.length / 1024).toFixed(1)} KB · ${theme.sections[S.SCREENS].length} screen(s) · ${glyphs} glyphs`);
    return source;
  }
  const busy = (on) => { btnPreview.disabled = on; btnSend.disabled = on || api === null; };

  btnPreview.onclick = async () => {
    busy(true);
    try { await compile(); } catch (e) { say(e.message, true); }
    busy(false);
  };
  btnSend.onclick = async () => {
    busy(true);
    try {
      const source = await compile();
      say('Sending…');
      const form = new FormData();
      form.append('theme', new Blob([bytes], { type: 'application/octet-stream' }), 'theme.stv');
      const r = await fetch(`${api}/api/theme2`, { method: 'POST', body: form });
      const d = await r.json();
      if (!r.ok) throw new Error(d.error || 'The device refused the theme');
      // Keep the source on the device too, so this editor can reopen it.
      const src = new FormData();
      src.append('source', new Blob([JSON.stringify(source, null, 1)], { type: 'application/json' }), 'source.json');
      await fetch(`${api}/api/theme2/source`, { method: 'POST', body: src });
      say(`Sent: ${(bytes.length / 1024).toFixed(1)} KB, now on the device.`);
      options.onInstalled?.();
    } catch (e) { say(e.message, true); }
    busy(false);
  };
  btnNext.onclick = () => {
    if (!preview.theme) return;
    const n = preview.theme.sections[S.SCREENS].length;
    preview.screen = (preview.screen + 1) % n;
    preview.shownAt = performance.now();
  };
  setInterval(() => { if (preview.theme) screenInfo.textContent = `screen ${preview.screen + 1} of ${preview.theme.sections[S.SCREENS].length}`; }, 500);
  btnReset.onclick = () => {
    if (text.value && !confirm('Replace the current source with the example theme?')) return;
    text.value = JSON.stringify(exampleTheme(), null, 1);
    btnPreview.onclick();
  };
  btnImage.onclick = () => {
    const file = imageFile.files[0], id = imageId.value.trim();
    if (!file || !/^[A-Za-z0-9_-]+$/.test(id)) { say('Pick an image file and give it an id (letters, digits, - and _).', true); return; }
    const reader = new FileReader();
    reader.onload = () => {
      try {
        const source = parse();
        source.images = { ...(source.images || {}), [id]: { src: reader.result } };
        text.value = JSON.stringify(source, null, 1);
        say(`Image '${id}' added: use it in a layer as { "type": "image", "image": "${id}", "x": 0, "y": 0 }.`);
      } catch (e) { say(e.message, true); }
    };
    reader.readAsDataURL(file);
  };

  // Start from the source stored on the device, else from the example.
  (async () => {
    let source = null;
    if (api !== null) {
      try { const r = await fetch(`${api}/api/theme2/source`); if (r.ok) source = await r.json(); } catch { /* none stored */ }
    }
    text.value = JSON.stringify(source || exampleTheme(), null, 1);
    btnPreview.onclick();
  })();
  return { preview, compile };
}
