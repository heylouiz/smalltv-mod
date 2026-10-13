// presets.js — images drawn by code (no files to host) and the example theme
// a new editor starts from.

const canvas = (w, h) => new OffscreenCanvas(w, h);

// Eight weather icons stacked as frames, in the builtin `wmoIcon` order:
// clear, partly cloudy, cloudy, fog, drizzle, rain, snow, thunderstorm.
export function weatherIcons(size = 32) {
  const c = canvas(size, size * 8), g = c.getContext('2d');
  g.scale(size / 32, size / 32);
  const sun = (y) => { g.fillStyle = '#ffd166'; g.beginPath(); g.arc(16, y + 16, 9, 0, 7); g.fill(); };
  const cloud = (y, color = '#cfd8e3') => {
    g.fillStyle = color; g.beginPath(); g.arc(12, y + 18, 7, 0, 7); g.arc(20, y + 15, 8, 0, 7); g.fill(); g.fillRect(6, y + 18, 20, 7);
  };
  const drops = (y, color) => { g.fillStyle = color; for (const x of [9, 16, 23]) g.fillRect(x, y + 27, 2, 4); };
  sun(0);
  sun(32); cloud(36);
  cloud(64);
  cloud(96, '#8a96a3'); g.fillStyle = '#8a96a3'; g.fillRect(6, 124, 20, 2);
  cloud(128); drops(128, '#7fb2ff');
  cloud(160, '#8a96a3'); drops(160, '#3d8bff');
  cloud(192); drops(192, '#ffffff');
  cloud(224, '#5c6670'); g.fillStyle = '#ffd166'; g.beginPath(); g.moveTo(17, 248); g.lineTo(13, 255); g.lineTo(16, 255); g.lineTo(14, 260); g.lineTo(20, 252); g.lineTo(17, 252); g.fill();
  return c;
}

export function gradient(from = '#0b1d33', to = '#03060c', w = 240, h = 240) {
  const c = canvas(w, h), g = c.getContext('2d');
  const grad = g.createLinearGradient(0, 0, 0, h);
  grad.addColorStop(0, from);
  grad.addColorStop(1, to);
  g.fillStyle = grad;
  g.fillRect(0, 0, w, h);
  return c;
}

export const PRESETS = { weatherIcons, gradient };

// The starting theme: a clock with the current weather, then a forecast.
export function exampleTheme() {
  const days = [0, 1, 2, 3, 4, 5];
  return {
    name: 'Clima semanal',
    background: '#04080f',
    fonts: {
      big: { google: 'Inter', weight: 700, size: 64, bpp: 4 },
      small: { google: 'Inter', weight: 500, size: 16, bpp: 4 },
    },
    images: {
      bg: { preset: 'gradient', from: '#0b1d33', to: '#03060c' },
      icons: { preset: 'weatherIcons', frames: 8 },
    },
    sources: {
      w: {
        url: 'http://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min,weather_code&timezone=auto',
        interval: 900,
        fields: {
          temp: 'current.temperature_2m', code: 'current.weather_code',
          max: 'daily.temperature_2m_max[0..6]', min: 'daily.temperature_2m_min[0..6]', dcode: 'daily.weather_code[0..6]',
        },
      },
    },
    screens: [
      {
        seconds: 20,
        layers: [
          { type: 'image', image: 'bg', x: 0, y: 0 },
          { type: 'text', font: 'small', x: 12, y: 14, w: 216, color: '#ffd166', text: '{CITY}' },
          { type: 'text', font: 'big', x: 0, y: 62, w: 240, align: 'center', color: '#ffffff', text: '{HH}:{MM}' },
          { type: 'text', font: 'small', x: 0, y: 140, w: 240, align: 'center', color: '#9fb4c8', text: '{WEEKDAY}, {DD} de {MONTH}' },
          { type: 'image', image: 'icons', x: 40, y: 182, frame: { value: 'w.code', table: 'wmoIcon' } },
          { type: 'text', font: 'small', x: 80, y: 182, w: 150, color: '#ffffff', text: '{w.temp|1}°C' },
          { type: 'text', font: 'small', x: 80, y: 200, w: 150, color: '#9fb4c8', text: '{w.code|wmoText}' },
        ],
      },
      {
        seconds: 10,
        background: '#03060c',
        layers: [
          { type: 'text', font: 'small', x: 12, y: 8, w: 216, color: '#ffd166', text: 'Próximos dias' },
          ...days.flatMap((d) => [
            { type: 'text', font: 'small', x: 14, y: 40 + d * 32, w: 60, color: '#ffffff', text: d ? `{WD|wd+${d}}` : 'Hoje' },
            { type: 'image', image: 'icons', x: 92, y: 34 + d * 32, frame: { value: `w.dcode${d}`, table: 'wmoIcon' } },
            { type: 'text', font: 'small', x: 130, y: 40 + d * 32, w: 96, align: 'right', color: '#cfd8e3', text: `{w.max${d}}° / {w.min${d}}°` },
          ]),
        ],
      },
    ],
  };
}
