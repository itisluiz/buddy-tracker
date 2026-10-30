// GPS view: summary stats, polar sky plot, per-satellite signal bars.

import { esc } from './util.js';

export const SYSTEMS = [
  { key: 'GP', name: 'GPS', color: 'var(--sys-gp)' },
  { key: 'BD', name: 'BeiDou', color: 'var(--sys-bd)' },
  { key: 'GL', name: 'GLONASS', color: 'var(--sys-gl)' },
  { key: 'GA', name: 'Galileo', color: 'var(--sys-ga)' },
];

const stat = (k, v) => `<div class="stat"><div class="k">${k}</div><div class="v">${v}</div></div>`;

export function renderGpsStats(el, self) {
  if (!self) {
    el.innerHTML = stat('Status', 'Not connected');
    return;
  }
  const t = self.utc?.length >= 6 ? `${self.utc.slice(0, 2)}:${self.utc.slice(2, 4)}:${self.utc.slice(4, 6)}` : '--:--:--';
  const inView = self.view.reduce((a, b) => a + b, 0);
  el.innerHTML = [
    stat('Fix', !self.gpsOk ? 'GPS silent' : self.fix ? (self.used >= 4 ? '3D' : '2D') : 'Searching'),
    stat('Satellites', `${self.used} / ${inView}`),
    stat('HDOP', self.hdop >= 99 ? '—' : self.hdop.toFixed(1)),
    stat('UTC', t),
    stat('Altitude', self.fix ? `${Math.round(self.alt)} m` : '—'),
    stat('Speed', self.fix ? `${self.spd.toFixed(1)} km/h` : '—'),
    stat('Course', self.fix && self.spd > 2 ? `${Math.round(self.crs)}°` : '—'),
    stat('Battery', self.bat >= 0 ? `${self.bat}%` : '—'),
    stat('Voltage', self.mv ? `${(self.mv / 1000).toFixed(2)} V` : '—'),
  ].join('');
}

export function renderSky(svg, legend, bars, sats) {
  const ring = (el) => `<circle class="ring" r="${((90 - el) / 90) * 100}"/>`;
  let html = ring(0) + ring(30) + ring(60) + '<line class="axis" x1="-100" y1="0" x2="100" y2="0"/><line class="axis" x1="0" y1="-100" x2="0" y2="100"/>';
  html += '<text x="0" y="-103" text-anchor="middle">N</text><text x="104" y="3">E</text><text x="0" y="110" text-anchor="middle">S</text><text x="-110" y="3">W</text>';
  for (const [sys, prn, el, az, snr, used] of sats) {
    if (!el && !az) continue; // no position known yet
    const r = ((90 - el) / 90) * 100, a = (az * Math.PI) / 180;
    const x = r * Math.sin(a), y = -r * Math.cos(a);
    const c = SYSTEMS[sys].color;
    const size = 5 + Math.min(snr, 50) / 12;
    html += `<g class="sat"><circle cx="${x}" cy="${y}" r="${size}" style="fill:${used ? c : 'var(--surface)'};stroke:${c};stroke-width:1.5"/>` +
      `<text x="${x}" y="${y - size - 2}" text-anchor="middle">${prn}</text></g>`;
  }
  svg.innerHTML = html;

  const counts = SYSTEMS.map((_, i) => sats.filter((s) => s[0] === i).length);
  legend.innerHTML = SYSTEMS.map((s, i) => (counts[i] ? `<span style="--c:${s.color}">${s.name} ${counts[i]}</span>` : '')).join('') +
    '<span class="muted" style="--c:transparent">filled = used in fix</span>';

  const sorted = [...sats].sort((a, b) => a[0] - b[0] || a[1] - b[1]);
  bars.innerHTML = sorted.length
    ? sorted.map(([sys, prn, , , snr, used]) =>
        `<div class="snr-bar ${used ? 'used' : ''}" style="--c:${SYSTEMS[sys].color}" title="${esc(SYSTEMS[sys].name)} ${prn}: ${snr || 0} dB-Hz">` +
        `<div class="val">${snr || ''}</div><div class="fill" style="height:${Math.min(100, ((snr || 0) / 50) * 100)}%"></div>` +
        `<div class="lbl">${SYSTEMS[sys].key[0]}${prn}</div></div>`).join('')
    : '<p class="muted">No satellites reported yet.</p>';
}
