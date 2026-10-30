// Leaflet map: me (with compass cone + accuracy), peers, waypoints, trails,
// and the range-test layer.

import { bearing, compass, distM, esc, fmtDist } from './util.js';

const L = window.L;

export class MapView {
  follow = true;
  #map;
  #me = null;
  #acc = null;
  #peers = new Map(); // id -> { marker, line }
  #wpLayer = L.layerGroup();
  #trailLayer = L.layerGroup();
  #rangeLayer = L.layerGroup();
  #trails = new Map(); // id -> polyline
  #heading = null;
  #self = null;

  constructor(el, onUserMove) {
    this.#map = L.map(el, { zoomControl: false, attributionControl: true }).setView([0, 0], 2);
    L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap contributors',
    }).addTo(this.#map);
    this.#trailLayer.addTo(this.#map);
    this.#wpLayer.addTo(this.#map);
    this.#map.on('dragstart', () => onUserMove?.());
  }

  invalidate() {
    this.#map.invalidateSize();
  }

  setSelf(self) {
    if (!self?.fix) return;
    const first = !this.#self;
    this.#self = self;
    const ll = [self.lat, self.lon];
    if (!this.#me) {
      this.#me = L.marker(ll, {
        icon: L.divIcon({ className: '', html: '<div class="me-marker"><div class="cone"></div><div class="dot"></div></div>', iconSize: [120, 120] }),
        zIndexOffset: 1000,
        interactive: false,
      }).addTo(this.#map);
      this.#acc = L.circle(ll, { radius: 10, color: '#22c55e', weight: 1, fillOpacity: 0.08, interactive: false }).addTo(this.#map);
    }
    this.#me.setLatLng(ll);
    this.#acc.setLatLng(ll).setRadius(Math.max(3, (self.hdop || 2) * 5));
    this.#applyHeading();
    if (first) this.#map.setView(ll, 17);
    else if (this.follow) this.#map.panTo(ll, { animate: true });
    for (const id of this.#peers.keys()) this.#updateLine(id);
  }

  setHeading(h) {
    this.#heading = h;
    this.#applyHeading();
  }

  #applyHeading() {
    const el = this.#me?.getElement()?.querySelector('.me-marker');
    if (!el) return;
    el.classList.toggle('has-heading', this.#heading != null);
    if (this.#heading != null) el.querySelector('.cone').style.transform = `rotate(${this.#heading}deg)`;
  }

  setPeer(p, { stale, sos }) {
    if (!p.fix) return;
    const ll = [p.lat, p.lon];
    let entry = this.#peers.get(p.id);
    const html = `<div class="peer-marker ${stale ? 'stale' : ''} ${sos ? 'sos' : ''}"><div class="pdot"></div><div class="plabel">${esc(p.id)}</div></div>`;
    if (!entry) {
      const marker = L.marker(ll, { icon: L.divIcon({ className: '', html, iconSize: [0, 0] }) }).addTo(this.#map);
      const line = L.polyline([ll, ll], { color: '#f97316', weight: 2, dashArray: '6 6', interactive: false }).addTo(this.#map);
      entry = { marker, line, html };
      this.#peers.set(p.id, entry);
    }
    entry.marker.setLatLng(ll);
    if (entry.html !== html) {
      entry.marker.setIcon(L.divIcon({ className: '', html, iconSize: [0, 0] }));
      entry.html = html;
    }
    entry.peer = p;
    this.#updateLine(p.id);
  }

  #updateLine(id) {
    const e = this.#peers.get(id);
    if (!e?.peer || !this.#self?.fix) return;
    const a = { lat: this.#self.lat, lon: this.#self.lon }, b = { lat: e.peer.lat, lon: e.peer.lon };
    e.line.setLatLngs([[a.lat, a.lon], [b.lat, b.lon]]);
    const label = `${fmtDist(distM(a, b))} ${compass(bearing(a, b))}`;
    if (!e.line.getTooltip()) e.line.bindTooltip(label, { permanent: true, direction: 'center', className: 'dist-label' });
    else e.line.setTooltipContent(label);
  }

  setWaypoints(wps) {
    this.#wpLayer.clearLayers();
    for (const [name, lat, lon] of wps) {
      L.marker([lat, lon], {
        icon: L.divIcon({ className: '', html: `<div class="wp-marker"><svg viewBox="0 0 24 32"><path class="pin-body" d="M12 31C12 31 2 19.5 2 12a10 10 0 0 1 20 0c0 7.5-10 19-10 19Z"/>` +
          `<circle class="pin-hole" cx="12" cy="12" r="4"/></svg><span class="wp-name">${esc(name)}</span></div>`, iconSize: [0, 0] }),
      }).addTo(this.#wpLayer);
    }
  }

  setTrails(trails) {
    for (const [id, pts] of Object.entries(trails)) {
      let line = this.#trails.get(id);
      if (!line) {
        line = L.polyline([], { color: id === 'me' ? '#22c55e' : '#f97316', weight: 3, opacity: 0.6, interactive: false });
        line.addTo(this.#trailLayer);
        this.#trails.set(id, line);
      }
      line.setLatLngs(pts);
    }
  }

  showLayer(name, on) {
    const layer = { trails: this.#trailLayer, waypoints: this.#wpLayer, range: this.#rangeLayer }[name];
    if (on) layer.addTo(this.#map);
    else layer.remove();
  }

  setRangeSamples(samples) {
    this.#rangeLayer.clearLayers();
    for (const s of samples) {
      const color = s.miss ? '#6b7280' : rssiColor(s.rssi);
      L.circleMarker([s.lat, s.lon], { radius: 6, color, weight: 1, fillColor: color, fillOpacity: 0.75 })
        .bindTooltip(s.miss ? `${s.peer}: missed` : `${s.peer}: ${s.rssi} dBm, SNR ${s.snr} dB, ${fmtDist(s.dist)}`)
        .addTo(this.#rangeLayer);
    }
  }

  focus(lat, lon) {
    this.follow = false;
    this.#map.setView([lat, lon], Math.max(this.#map.getZoom(), 16));
  }

  fitAll() {
    const pts = [];
    if (this.#self?.fix) pts.push([this.#self.lat, this.#self.lon]);
    for (const e of this.#peers.values()) if (e.peer) pts.push([e.peer.lat, e.peer.lon]);
    this.#wpLayer.eachLayer((m) => pts.push(m.getLatLng()));
    if (pts.length === 1) this.#map.setView(pts[0], 17);
    else if (pts.length) this.#map.fitBounds(L.latLngBounds(pts).pad(0.25), { maxZoom: 18 });
  }

  recenter() {
    if (this.#self?.fix) this.#map.panTo([this.#self.lat, this.#self.lon]);
  }
}

// -120 dBm red .. -60 dBm green
export function rssiColor(rssi) {
  const t = Math.max(0, Math.min(1, (rssi + 120) / 60));
  return `hsl(${Math.round(t * 120)} 80% 45%)`;
}
