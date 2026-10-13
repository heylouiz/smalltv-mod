// A two-screen weather clock, the shape the editor's presets produce. Shared
// by the compiler, renderer and (later) device parity tests.
import { requiredGlyphs } from '../src/compile.js';
import { blockFont, stripes, solidImage } from './fixtures.js';

export const source = {
  name: 'Clima semanal',
  background: '#04080f',
  fonts: { big: { family: 'Test', size: 48, bpp: 4 }, small: { family: 'Test', size: 16, bpp: 1 } },
  images: { bg: {}, icons: { frames: 8 } },
  sources: {
    w: {
      url: 'http://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min,weather_code&timezone=auto',
      interval: 900,
      fields: { temp: 'current.temperature_2m', code: 'current.weather_code', max: 'daily.temperature_2m_max[0..6]', min: 'daily.temperature_2m_min[0..6]', dcode: 'daily.weather_code[0..6]' },
    },
  },
  screens: [
    {
      seconds: 15,
      layers: [
        { type: 'image', image: 'bg', x: 0, y: 0 },
        { type: 'text', font: 'big', x: 0, y: 70, w: 240, align: 'center', color: '#ffffff', text: '{HH}:{MM}' },
        { type: 'text', font: 'small', x: 0, y: 130, w: 240, align: 'center', color: '#9fb4c8', text: '{WEEKDAY}, {DD} de {MONTH}' },
        { type: 'text', font: 'small', x: 8, y: 8, w: 160, color: '#ffd166', text: '{CITY}' },
        { type: 'image', image: 'icons', x: 180, y: 180, frame: { value: 'w.code', table: 'wmoIcon' } },
        { type: 'text', font: 'small', x: 8, y: 200, w: 160, color: '#ffffff', text: '{w.temp|1}°C {w.code|wmoText}' },
      ],
    },
    {
      seconds: 10,
      background: '#000000',
      layers: [
        { type: 'rect', x: 4, y: 4, w: 232, h: 232, color: '#102030', radius: 12 },
        { type: 'text', font: 'small', x: 10, y: 20, w: 60, color: '#ffffff', text: 'Hoje' },
        { type: 'text', font: 'small', x: 80, y: 20, w: 150, align: 'right', color: '#ffffff', text: '{w.max0}° / {w.min0}°' },
        { type: 'text', font: 'small', x: 10, y: 50, w: 60, color: '#ffffff', text: '{WD|wd+1}' },
        { type: 'text', font: 'small', x: 80, y: 50, w: 150, align: 'right', color: '#ffffff', text: '{w.max1}° / {w.min1}°' },
        { type: 'circle', x: 110, y: 200, size: 20, color: '#ffd166' },
      ],
    },
  ],
};

export function assets() {
  const need = requiredGlyphs(source);
  return {
    fonts: {
      big: blockFont([...need.big, 0x3f], { w: 20, h: 40, ascent: 36, advance: 22, lineHeight: 48 }),
      small: blockFont([...need.small, 0x3f], { w: 6, h: 10, ascent: 9, advance: 7, lineHeight: 14 }),
    },
    images: {
      bg: stripes(240, 240, [[4, 8, 15, 255], [10, 20, 40, 255]]),
      icons: stripes(32, 256, [[255, 210, 0, 255], [0, 0, 0, 0]]),
    },
  };
}

// An Open-Meteo response trimmed to what the sample fetches.
export const forecast = {
  latitude: -22.56, longitude: -47.4,
  current_units: { temperature_2m: '°C' },
  current: { time: '2026-10-13T10:00', temperature_2m: 24.86, weather_code: 61 },
  daily: {
    time: ['2026-10-13', '2026-10-14', '2026-10-15', '2026-10-16', '2026-10-17', '2026-10-18', '2026-10-19'],
    temperature_2m_max: [29.4, 31, 27.6, 25.1, 26, 28.2, 30.5],
    temperature_2m_min: [17.2, 18.5, 16, 15.9, 15, 16.4, 18],
    weather_code: [61, 3, 0, 95, 2, 1, 80],
  },
};
