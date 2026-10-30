// Range test: log each beacon heard (and missed) at this board's position.

import { distM, download, fmtDist, store } from './util.js';

const MISS_AFTER_MS = 12000; // beacons come every ~5-6 s

export class RangeTest {
  running = false;
  samples = store('range:samples') || [];
  #lastHeard = new Map();
  #lastMissLog = new Map();

  toggle() {
    this.running = !this.running;
    this.#lastHeard.clear();
    return this.running;
  }

  // A fresh beacon from a peer while we know where we are
  heard(self, peer) {
    if (!this.running || !self?.fix || !peer.fix) return false;
    this.#lastHeard.set(peer.id, Date.now());
    this.#push({ t: Date.now(), peer: peer.id, lat: self.lat, lon: self.lon, rssi: peer.rssi, snr: peer.snr,
      dist: distM(self, peer) });
    return true;
  }

  // Called every second: log a miss when a peer we were hearing goes quiet
  tick(self) {
    if (!this.running || !self?.fix) return false;
    let added = false;
    for (const [id, at] of this.#lastHeard) {
      const now = Date.now();
      if (now - at > MISS_AFTER_MS && now - (this.#lastMissLog.get(id) || 0) > MISS_AFTER_MS) {
        this.#lastMissLog.set(id, now);
        this.#push({ t: now, peer: id, lat: self.lat, lon: self.lon, miss: true });
        added = true;
      }
    }
    return added;
  }

  #push(s) {
    this.samples.push(s);
    if (this.samples.length > 20000) this.samples.shift();
    store('range:samples', this.samples);
  }

  clear() {
    this.samples = [];
    store('range:samples', []);
  }

  summary() {
    const heard = this.samples.filter((s) => !s.miss);
    const far = heard.reduce((m, s) => Math.max(m, s.dist), 0);
    const weakest = heard.reduce((m, s) => Math.min(m, s.rssi), 0);
    return { heard: heard.length, missed: this.samples.length - heard.length, far: heard.length ? fmtDist(far) : '—',
      weakest: heard.length ? `${weakest} dBm` : '—' };
  }

  exportCsv() {
    const rows = ['time,peer,lat,lon,heard,rssi,snr,distance_m'];
    for (const s of this.samples)
      rows.push([new Date(s.t).toISOString(), s.peer, s.lat.toFixed(7), s.lon.toFixed(7), s.miss ? 0 : 1,
        s.rssi ?? '', s.snr ?? '', s.dist != null ? s.dist.toFixed(1) : ''].join(','));
    download(`range-test-${new Date().toISOString().slice(0, 10)}.csv`, rows.join('\n'), 'text/csv');
  }
}
