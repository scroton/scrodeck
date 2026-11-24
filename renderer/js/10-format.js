'use strict';
/* global Terminal, FitAddon, WebglAddon, deck */
// Classic scripts share one global scope; load order = the numeric prefix (see index.html).
// ---------------------------------------------------------------- formatting
function fmtClock(d) { const h = d.getHours() % 12 || 12; return `${h}:${String(d.getMinutes()).padStart(2, '0')}`; }
function fmtAge(ms) {
  const s = Math.max(0, Math.floor(ms / 1000));
  if (s < 60) return 'now';
  const m = Math.floor(s / 60); if (m < 60) return `${m}m`;
  const h = Math.floor(m / 60); if (h < 24) return `${h}h`;
  return `${Math.floor(h / 24)}d`;
}
function fmtResetTime(iso) {
  if (!iso) return '—';
  const d = new Date(iso); if (isNaN(d)) return '—';
  const h = d.getHours(); const ap = h >= 12 ? 'p' : 'a';
  return `${h % 12 || 12}:${String(d.getMinutes()).padStart(2, '0')}${ap}`;
}
function fmtResetDay(iso) {
  if (!iso) return '—';
  const d = new Date(iso); if (isNaN(d)) return '—';
  return `${d.getMonth() + 1}.${d.getDate()}`;
}
// countdown labels: hours until a reset ("2h 15m", "42m"), days until a reset ("4d", hours under 24 h)
function fmtUntilHours(iso) {
  if (!iso) return '—';
  const ms = new Date(iso) - Date.now(); if (isNaN(ms)) return '—';
  if (ms <= 0) return 'now';
  const m = Math.ceil(ms / 60000), h = Math.floor(m / 60);
  return h >= 1 ? `${h}h ${String(m % 60).padStart(2, '0')}m` : `${m}m`;
}
function fmtUntilDays(iso) {
  if (!iso) return '—';
  const ms = new Date(iso) - Date.now(); if (isNaN(ms)) return '—';
  if (ms < 24 * 3600000) return fmtUntilHours(iso);
  return `${Math.ceil(ms / 86400000)}d`;
}
// bar fills with USAGE: empty and green at 0 %, full and red at 100 %
function barColor(used) {
  const u = Math.max(0, Math.min(100, used));
  const hue = Math.round(120 * (1 - u / 100));
  return `linear-gradient(90deg, hsl(${hue} 68% 40%), hsl(${hue} 70% 50%))`;
}

// Fable bar: the 2049 palette. Cyan at 0 % through electric blue at 50 % into magenta-violet at 100 %.
const FABLE_STOPS = [[34, 211, 238], [59, 130, 246], [168, 85, 247], [217, 70, 239]];   // #22d3ee → #3b82f6 → #a855f7 → #d946ef
function fableRGB(t) {
  const n = FABLE_STOPS.length - 1, x = Math.max(0, Math.min(1, t)) * n, i = Math.min(n - 1, Math.floor(x)), f = x - i;
  const a = FABLE_STOPS[i], b = FABLE_STOPS[i + 1];
  return a.map((v, k) => Math.round(v + (b[k] - v) * f));
}
function fableColor(used) {
  const t = Math.max(0, Math.min(100, used)) / 100;
  const c1 = fableRGB(Math.max(0, t - 0.15)), c2 = fableRGB(t);         // the fill itself shades from a little behind to the current point
  return `linear-gradient(90deg, rgb(${c1.join(' ')}), rgb(${c2.join(' ')}))`;
}
