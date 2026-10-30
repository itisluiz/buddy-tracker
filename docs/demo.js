// Demo mode (index.html?demo): feeds made-up board data so the UI can be previewed without hardware.
const tab = new URLSearchParams(location.search).get('tab') || 'map';
const send = (o) => window.buddy.link.dispatchEvent(new CustomEvent('line', { detail: o }));
const wait = () => new Promise((r) => setTimeout(r, 50));
(async () => {
  while (!window.buddy) await wait();
  window.buddy.link.dispatchEvent(new CustomEvent('state', { detail: 'on' }));
  send({ t: 'self', id: '3D2C', fix: 1, lat: -25.424643, lon: -49.251363, alt: 903.6, spd: 0.4, crs: 0, hdop: 1.2, used: 7, view: [7, 3, 0, 0], utc: '024429', date: '301026', bat: 82, mv: 3987, gpsOk: 1 });
  send({ t: 'peer', id: '8424', fix: 1, lat: -25.4239, lon: -49.2502, sats: 6, bat: 64, hdop: 1.6, rssi: -88, snr: 7.5, age: 0 });
  send({ t: 'peer', id: '51A0', fix: 0, lat: 0, lon: 0, sats: 0, bat: -1, hdop: 99, rssi: -112, snr: -6, age: 45 });
  send({ t: 'sky', s: [[0, 5, 62, 40, 41, 1], [0, 13, 35, 120, 33, 1], [0, 15, 20, 210, 28, 1], [0, 18, 70, 300, 44, 1], [0, 23, 12, 330, 18, 0], [0, 24, 48, 80, 36, 1], [0, 29, 8, 160, 0, 0], [1, 7, 55, 270, 30, 1], [1, 10, 25, 30, 22, 1], [1, 21, 15, 190, 14, 0]] });
  send({ t: 'cfg', leashM: 300, leashMin: 5, screenS: 60, northRight: 1, bt: 1, trk: 412 });
  send({ t: 'wps', w: [['CAR', -25.4251, -49.2521], ['CAMP', -25.4238, -49.2528]] });
  send({ t: 'msg', from: '8424', me: 0, seq: 3, st: 'rx', sos: 0, age: 120, text: 'WHERE R U' });
  send({ t: 'msg', from: '3D2C', me: 1, seq: 9, st: 'ok', sos: 0, age: 90, text: 'BY THE GATE' });
  send({ t: 'msg', from: '3D2C', me: 1, seq: 10, st: 'wait', sos: 0, age: 2, text: 'COMING' });
  send({ t: 'scan', from: '8424', kind: 'wifi', part: 0, total: 1, e: [[-48, 6, 3, 'Casa 51 - 2.4GHz'], [-71, 11, 7, 'VIVO-1234'], [-83, 1, 0, 'Free WiFi'], [-90, 6, 3, '']] });
  send({ t: 'scan', from: '8424', kind: 'ble', part: 0, total: 1, e: [[-60, 76, 'iPhone'], [-77, 117, ''], [-85, -1, 'Mi Band 7']] });
  window.buddy.state.heading = 45;
  document.querySelector(`.tabs button[data-tab="${tab}"]`).click();
  if (tab === 'tools') document.querySelectorAll('details').forEach((d) => (d.open = true));
})();
