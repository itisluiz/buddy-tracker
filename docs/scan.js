// Wi-Fi / BLE survey results, merged from the board's (possibly multi-part) replies.

import { bars, esc, fmtAge } from './util.js';

const AUTH = ['Open', 'WEP', 'WPA', 'WPA2', 'WPA/WPA2', 'WPA2-Ent', 'WPA3', 'WPA2/WPA3', 'WAPI', 'OWE', 'WPA3-Ent'];

// Bluetooth SIG company identifiers (the common ones)
const COMPANIES = {
  0x0002: 'Intel', 0x0006: 'Microsoft', 0x000a: 'Qualcomm', 0x000f: 'Broadcom', 0x0046: 'MediaTek',
  0x004c: 'Apple', 0x0059: 'Nordic', 0x0075: 'Samsung', 0x0087: 'Garmin', 0x00e0: 'Google',
  0x0157: 'Huami', 0x0171: 'Amazon', 0x02e5: 'Espressif', 0x038f: 'Xiaomi', 0x0499: 'Ruuvi',
};

export class ScanView {
  #boards = new Map(); // board id -> { at, wifi: Map(part -> entries), ble: Map, totals }
  #tab = new Map();    // board id -> 'wifi' | 'ble'
  #sort = { key: 'rssi', dir: -1 };

  constructor(el) {
    this.el = el;
    el.addEventListener('click', (e) => {
      const seg = e.target.closest('[data-seg]');
      if (seg) {
        this.#tab.set(seg.dataset.board, seg.dataset.seg);
        this.render();
      }
      const th = e.target.closest('th[data-sort]');
      if (th) {
        const key = th.dataset.sort;
        this.#sort = { key, dir: this.#sort.key === key ? -this.#sort.dir : key === 'rssi' ? -1 : 1 };
        this.render();
      }
    });
  }

  pending(target) {
    this.waiting = { target, at: Date.now() };
    this.render();
  }

  add(line) {
    let b = this.#boards.get(line.from);
    // a part 0 for a kind starts a fresh result set for that kind
    if (!b) this.#boards.set(line.from, (b = { at: Date.now(), wifi: new Map(), ble: new Map() }));
    if (line.part === 0) b[line.kind].clear();
    b[line.kind].set(line.part, line.e);
    b.at = Date.now();
    this.waiting = null;
    this.render();
  }

  render() {
    const parts = [];
    if (this.waiting && Date.now() - this.waiting.at < 30000)
      parts.push(`<p class="muted">Scanning ${this.waiting.target === 'me' ? 'here' : esc(this.waiting.target)}…</p>`);
    for (const [id, b] of this.#boards) {
      const tab = this.#tab.get(id) || (b.wifi.size ? 'wifi' : 'ble');
      const entries = [...b[tab].values()].flat();
      parts.push(`<div class="scan-board"><h4><span>${esc(id)} <span class="muted small">${fmtAge(Date.now() - b.at)}</span></span>
        <span class="seg"><button data-seg="wifi" data-board="${esc(id)}" class="${tab === 'wifi' ? 'active' : ''}">Wi-Fi ${[...b.wifi.values()].flat().length}</button>
        <button data-seg="ble" data-board="${esc(id)}" class="${tab === 'ble' ? 'active' : ''}">BLE ${[...b.ble.values()].flat().length}</button></span></h4>
        ${tab === 'wifi' ? this.#wifiTable(entries) : this.#bleTable(entries)}</div>`);
    }
    this.el.innerHTML = parts.join('') || '<p class="muted">No scans yet.</p>';
  }

  #sorted(rows, keyFn) {
    const { key, dir } = this.#sort;
    return [...rows].sort((a, b) => {
      const x = keyFn(a, key), y = keyFn(b, key);
      return (x > y ? 1 : x < y ? -1 : 0) * dir;
    });
  }

  #wifiTable(rows) {
    if (!rows.length) return '<p class="muted">Nothing found.</p>';
    const key = (r, k) => ({ rssi: r[0], ch: r[1], sec: r[2], name: (r[3] || '').toLowerCase() })[k];
    return `<table class="list"><tr><th data-sort="rssi">Signal</th><th data-sort="name">Network</th><th data-sort="ch">Ch</th><th data-sort="sec">Security</th></tr>
      ${this.#sorted(rows, key).map(([rssi, ch, auth, ssid]) => `<tr><td class="num">${bars(rssi, -95, -40)} ${rssi}</td>
        <td>${ssid ? esc(ssid) : '<span class="muted">(hidden)</span>'}</td><td class="num">${ch}</td>
        <td>${auth === 0 ? '<span class="tag warn">Open</span>' : esc(AUTH[auth] || `#${auth}`)}</td></tr>`).join('')}</table>`;
  }

  #bleTable(rows) {
    if (!rows.length) return '<p class="muted">Nothing found.</p>';
    const key = (r, k) => ({ rssi: r[0], name: (r[2] || '').toLowerCase(), maker: r[1] })[k];
    return `<table class="list"><tr><th data-sort="rssi">Signal</th><th data-sort="name">Name</th><th data-sort="maker">Maker</th></tr>
      ${this.#sorted(rows, key).map(([rssi, company, name]) => `<tr><td class="num">${bars(rssi, -100, -45)} ${rssi}</td>
        <td>${name ? esc(name) : '<span class="muted">(no name)</span>'}</td>
        <td>${company < 0 ? '—' : esc(COMPANIES[company] || `0x${company.toString(16).padStart(4, '0')}`)}</td></tr>`).join('')}</table>`;
  }
}
