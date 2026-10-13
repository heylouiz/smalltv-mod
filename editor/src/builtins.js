// builtins.js — tables, formats and clock tokens every theme gets for free.
// A theme can override any of them by declaring the same ID.
import { CLOCK } from './format.js';

const list = (values) => ({ entries: values.map((value, i) => ({ from: i, to: i, value })) });
const listFrom1 = (values) => ({ entries: values.map((value, i) => ({ from: i + 1, to: i + 1, value })) });

// Open-Meteo / WMO weather codes grouped into 8 icon slots: a sprite sheet
// with frames in this order can be driven by `wmoIcon` directly.
// 0 clear, 1 partly cloudy, 2 cloudy, 3 fog, 4 drizzle, 5 rain, 6 snow, 7 thunderstorm
const WMO_GROUPS = [
  [0, 0, 0], [1, 2, 1], [3, 3, 2], [45, 48, 3], [51, 57, 4], [61, 67, 5], [80, 82, 5],
  [71, 77, 6], [85, 86, 6], [95, 99, 7],
];
const WMO_TEXT = [
  [0, 0, 'Céu limpo'], [1, 1, 'Poucas nuvens'], [2, 2, 'Parcialmente nublado'], [3, 3, 'Nublado'],
  [45, 48, 'Neblina'], [51, 55, 'Garoa'], [56, 57, 'Garoa congelante'], [61, 61, 'Chuva fraca'],
  [63, 63, 'Chuva'], [65, 65, 'Chuva forte'], [66, 67, 'Chuva congelante'], [71, 75, 'Neve'],
  [77, 77, 'Granizo fino'], [80, 81, 'Pancadas de chuva'], [82, 82, 'Pancadas fortes'],
  [85, 86, 'Pancadas de neve'], [95, 95, 'Trovoadas'], [96, 99, 'Trovoadas com granizo'],
];

export const TABLES = {
  weekdays: list(['Dom', 'Seg', 'Ter', 'Qua', 'Qui', 'Sex', 'Sáb']),
  weekdaysLong: list(['Domingo', 'Segunda', 'Terça', 'Quarta', 'Quinta', 'Sexta', 'Sábado']),
  months: listFrom1(['Jan', 'Fev', 'Mar', 'Abr', 'Mai', 'Jun', 'Jul', 'Ago', 'Set', 'Out', 'Nov', 'Dez']),
  monthsLong: listFrom1(['Janeiro', 'Fevereiro', 'Março', 'Abril', 'Maio', 'Junho', 'Julho', 'Agosto',
    'Setembro', 'Outubro', 'Novembro', 'Dezembro']),
  ampm: list(['AM', 'PM']),
  wmoIcon: { entries: WMO_GROUPS.map(([from, to, value]) => ({ from, to, value })), default: 1 },
  wmoText: { entries: WMO_TEXT.map(([from, to, value]) => ({ from, to, value })), default: '' },
};

export const FORMATS = {
  int: {},
  '1': { decimals: 1 },
  '2': { width: 2 },
  weekday: { table: 'weekdays' },
  weekdayLong: { table: 'weekdaysLong' },
  month: { table: 'months' },
  monthLong: { table: 'monthsLong' },
  wmoText: { table: 'wmoText' },
  ampm: { table: 'ampm' },
  // The weekday N days from today, for forecast columns: {WD|wd+1} ... {WD|wd+6}.
  ...Object.fromEntries([1, 2, 3, 4, 5, 6].map((n) => [`wd+${n}`, { add: n, mod: 7, table: 'weekdays' }])),
};

// Clock tokens: the slot they read and the format they use unless the
// template names another one.
export const CLOCK_TOKENS = {
  HH: [CLOCK.H24, '2'], hh: [CLOCK.H12, 'int'], MM: [CLOCK.MIN, '2'], SS: [CLOCK.SEC, '2'],
  DD: [CLOCK.DAY, '2'], MO: [CLOCK.MONTH, '2'], YYYY: [CLOCK.YEAR, 'int'],
  MON: [CLOCK.MONTH, 'month'], MONTH: [CLOCK.MONTH, 'monthLong'],
  WD: [CLOCK.WEEKDAY, 'weekday'], WEEKDAY: [CLOCK.WEEKDAY, 'weekdayLong'], AMPM: [CLOCK.AMPM, 'ampm'],
};
