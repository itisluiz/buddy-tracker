import { BuddyLink, supported } from './ble.js';
import { MapView } from './map.js';
import { RangeTest } from './range.js';
import { ScanView } from './scan.js';
import { renderGpsStats, renderSky } from './sky.js';
import { bars, bearing, compass, distM, download, esc, fmtAge, fmtDist, store } from './util.js';

const $ = (id) => document.getElementById(id);

// ---------------------------------------------------------------- state

const link = new BuddyLink();
const state = {
  boardId: null,
  self: null,
  peers: new Map(), // id -> peer line + heardAt
  msgs: new Map(),  // "from:seq" -> { from, seq, me, st, sos, text, at }
  wps: [],
  cfg: null,
  sky: [],
  heading: null,
  sosFrom: null,
  trails: {},       // id ('me' or peer id) -> [[lat, lon], ...]
  unread: 0,
  track: null,      // GPX download in progress
};

const map = new MapView($('map'), () => setFollow(false));
const scans = new ScanView($('scanResults'));
const range = new RangeTest();
let activeTab = 'map';

// ---------------------------------------------------------------- connection

if (!supported()) {
  $('unsupported').hidden = false;
  $('connBtn').disabled = true;
}

$('connBtn').addEventListener('click', async () => {
  if (link.state === 'on' || link.state === 'reconnecting') {
    if (confirm(`Disconnect from ${state.boardId ?? 'the board'}?`)) link.disconnect();
    return;
  }
  if (link.state !== 'off') return;
  try {
    await link.connect();
  } catch (e) {
    if (e.name !== 'NotFoundError') toast(`Couldn't connect: ${e.message}`, true); // NotFoundError = picker cancelled
    if (link.state !== 'on') link.disconnect();
  }
});

link.addEventListener('state', (e) => {
  const s = e.detail;
  const btn = $('connBtn');
  btn.dataset.state = s === 'on' ? 'on' : s === 'off' ? 'off' : 'busy';
  btn.textContent = { off: 'Connect', connecting: 'Connecting…', pairing: 'Enter code shown on board', on: state.boardId ?? 'Connected', reconnecting: 'Reconnecting…' }[s];
  const on = s === 'on';
  for (const id of ['sosBtn', 'chatText', 'chatSend', 'scanBtn', 'markBtn', 'gpxBtn', 'gpxClearBtn', 'settingsSave', 'forgetBtn'])
    $(id).disabled = !on;
  if (s === 'off') $('boardName').textContent = 'No board connected';
  if (s === 'pairing') toast('First time? Type the 6-digit code shown on the board when your phone asks.');
});

link.addEventListener('error', (e) => toast(`Bluetooth error: ${e.detail?.message ?? e.detail}`, true));

link.addEventListener('line', (e) => {
  const m = e.detail;
  switch (m.t) {
    case 'self': return onSelf(m);
    case 'peer': return onPeer(m);
    case 'sky': state.sky = m.s; return activeTab === 'sky' && renderSkyTab();
    case 'msg': return onMsg(m);
    case 'wps': state.wps = m.w; map.setWaypoints(m.w); return renderWaypoints();
    case 'cfg': state.cfg = m; return renderSettings();
    case 'alert': return onAlert(m);
    case 'scan': return scans.add(m);
    case 'trk': case 'trkp': case 'trkend': return onTrack(m);
  }
});

// ---------------------------------------------------------------- incoming

function onSelf(m) {
  const first = state.boardId !== m.id;
  state.self = m;
  if (first) {
    state.boardId = m.id;
    state.trails = store(`trails:${m.id}`) || {};
    loadChat();
    map.setTrails(state.trails);
    $('connBtn').textContent = m.id;
    $('boardName').textContent = `Board ${m.id}`;
  }
  $('boardName').textContent = `Board ${m.id} · ${m.fix ? `${m.used} sats` : 'no GPS fix'}`;
  const bat = $('chipBattery');
  bat.hidden = m.bat < 0;
  bat.textContent = `${m.bat}%`;
  bat.title = `${(m.mv / 1000).toFixed(2)} V`;
  const sats = $('chipSats');
  sats.hidden = false;
  sats.textContent = m.fix ? `${m.used} sat` : 'no fix';
  if (m.fix) {
    addTrail('me', m);
    map.setSelf(m);
  }
  if (activeTab === 'sky') renderGpsStats($('gpsStats'), m);
  if (range.tick(m) && $('layerRange').checked) map.setRangeSamples(range.samples);
}

function onPeer(m) {
  const p = { ...m, heardAt: Date.now() - m.age * 1000 };
  state.peers.set(p.id, p);
  if (p.fix) addTrail(p.id, p);
  map.setPeer(p, { stale: m.age > 30, sos: state.sosFrom === p.id });
  if (m.age <= 1 && range.heard(state.self, p)) {
    if ($('layerRange').checked) map.setRangeSamples(range.samples);
    renderRange();
  }
  updateScanTargets();
  if (activeTab === 'peers') renderPeers();
}

function onMsg(m) {
  const key = `${m.from}:${m.seq}`;
  const prev = state.msgs.get(key);
  const fresh = !prev || prev.text !== m.text; // seq wraps at 256: different text = different message
  const msg = { ...m, at: fresh ? Date.now() - m.age * 1000 : prev.at };
  state.msgs.set(key, msg);
  if (fresh && !m.me && m.age < 5 && activeTab !== 'chat') {
    state.unread++;
    updateBadge();
  }
  saveChat();
  renderChat();
}

function onAlert(m) {
  if (m.kind === 'sos') {
    state.sosFrom = m.id;
    const p = state.peers.get(m.id);
    const where = p?.fix && state.self?.fix ? ` — ${fmtDist(distM(state.self, p))} ${compass(bearing(state.self, p))}` : '';
    $('sosText').innerHTML = `<p><b>${esc(m.id)}</b> sent an SOS${esc(where)}.</p>`;
    $('sosModal').hidden = false;
    navigator.vibrate?.([600, 200, 600, 200, 600]);
    beep(5);
    if (p) map.setPeer(p, { stale: false, sos: true });
  } else if (m.kind === 'far' || m.kind === 'silent') {
    toast(`Leash: ${m.text}`, true);
    navigator.vibrate?.([300, 150, 300]);
    beep(2);
  } else {
    toast(m.text);
  }
}

// ---------------------------------------------------------------- trails

let trailSaveTimer = null;
function addTrail(id, p) {
  const pts = (state.trails[id] ||= []);
  const last = pts[pts.length - 1];
  if (last && distM({ lat: last[0], lon: last[1] }, p) < 5) return;
  pts.push([+p.lat.toFixed(6), +p.lon.toFixed(6)]);
  if (pts.length > 3000) pts.shift();
  if ($('layerTrails').checked) map.setTrails({ [id]: pts });
  clearTimeout(trailSaveTimer);
  trailSaveTimer = setTimeout(() => store(`trails:${state.boardId}`, state.trails), 5000);
}

$('clearTrailsBtn').addEventListener('click', () => {
  for (const id in state.trails) state.trails[id] = [];
  map.setTrails(state.trails);
  store(`trails:${state.boardId}`, state.trails);
});

// ---------------------------------------------------------------- tabs

$('tabs').addEventListener('click', (e) => {
  const b = e.target.closest('button[data-tab]');
  if (b) showTab(b.dataset.tab);
});

function showTab(tab) {
  activeTab = tab;
  for (const b of $('tabs').querySelectorAll('button')) b.classList.toggle('active', b.dataset.tab === tab);
  for (const p of document.querySelectorAll('.panel')) p.classList.toggle('active', p.id === `tab-${tab}`);
  if (tab === 'map') map.invalidate();
  if (tab === 'peers') renderPeers();
  if (tab === 'sky') renderSkyTab();
  if (tab === 'chat') {
    state.unread = 0;
    updateBadge();
    renderChat();
  }
  if (tab === 'tools') {
    renderRange();
    renderWaypoints();
  }
}

// ---------------------------------------------------------------- map controls

function setFollow(on) {
  map.follow = on;
  $('followBtn').setAttribute('aria-pressed', on);
}
$('followBtn').addEventListener('click', () => {
  setFollow(!map.follow);
  if (map.follow) map.recenter();
});
$('fitBtn').addEventListener('click', () => {
  setFollow(false);
  map.fitAll();
});
$('layersBtn').addEventListener('click', () => ($('layersMenu').hidden = !$('layersMenu').hidden));
$('layerTrails').addEventListener('change', (e) => map.showLayer('trails', e.target.checked));
$('layerWaypoints').addEventListener('change', (e) => map.showLayer('waypoints', e.target.checked));
$('layerRange').addEventListener('change', (e) => {
  map.showLayer('range', e.target.checked);
  if (e.target.checked) map.setRangeSamples(range.samples);
});

// ---------------------------------------------------------------- compass

let smoothed = null;
$('compassBtn').addEventListener('click', async () => {
  try {
    if (typeof DeviceOrientationEvent?.requestPermission === 'function') await DeviceOrientationEvent.requestPermission();
  } catch {
    return toast('Compass permission denied', true);
  }
  const handler = (e) => {
    let h = e.webkitCompassHeading ?? (e.absolute || e.type === 'deviceorientationabsolute' ? 360 - e.alpha : null);
    if (h == null || Number.isNaN(h)) return;
    h = (h + (screen.orientation?.angle || 0)) % 360;
    // low-pass on the circle so 359 -> 1 doesn't swing the long way round
    smoothed = smoothed == null ? h : (smoothed + ((((h - smoothed + 540) % 360) - 180) * 0.25) + 360) % 360;
    state.heading = smoothed;
  };
  if ('ondeviceorientationabsolute' in window) window.addEventListener('deviceorientationabsolute', handler);
  else window.addEventListener('deviceorientation', handler);
  $('compassHint').hidden = true;
  toast('Compass on. Hold the phone flat; wave it in a figure-8 if the heading drifts.');
});

setInterval(() => {
  if (state.heading == null) return;
  map.setHeading(state.heading);
  if (activeTab === 'peers') updateArrows();
}, 100);

// ---------------------------------------------------------------- peers

function relTurn(b) {
  const d = ((b - state.heading + 540) % 360) - 180;
  if (Math.abs(d) < 15) return 'straight ahead';
  if (Math.abs(d) > 165) return 'behind you';
  return `turn ${Math.round(Math.abs(d))}° ${d < 0 ? 'left' : 'right'}`;
}

function renderPeers() {
  const list = $('peerList');
  const peers = [...state.peers.values()].sort((a, b) => b.heardAt - a.heardAt);
  $('peerEmpty').hidden = peers.length > 0;
  $('headingNote').textContent = state.heading == null
    ? 'Arrows point relative to north. Tap "Enable compass" on the map to point them where you face.'
    : 'Arrows point where to walk, relative to where your phone faces.';
  const self = state.self;
  list.innerHTML = peers.map((p) => {
    const age = Date.now() - p.heardAt;
    const stale = age > 30000;
    const hasPos = p.fix && self?.fix;
    const d = hasPos ? distM(self, p) : null;
    const b = hasPos ? bearing(self, p) : null;
    const leash = state.cfg?.leashM && d > state.cfg.leashM;
    return `<div class="card peer-card ${stale ? 'stale' : ''}" data-id="${esc(p.id)}" data-bearing="${b ?? ''}">
      <div class="arrow-dial"><span class="dial-n">${state.heading == null ? 'N' : 'ahead'}</span>
        <svg viewBox="0 0 96 96"><path d="M48 14 63 62 48 54 33 62Z" style="fill:${hasPos ? 'var(--accent)' : 'var(--border)'};stroke:none"/></svg></div>
      <div>
        <div class="peer-head"><span class="peer-id">${esc(p.id)} ${state.sosFrom === p.id ? '<span class="tag warn">SOS</span>' : ''}</span>
          <span class="peer-dist">${d != null ? fmtDist(d) : '—'}</span></div>
        <div class="peer-sub turn">${hasPos ? `${compass(b)} · ${Math.round(b)}°${state.heading != null ? ` · ${relTurn(b)}` : ''}` : p.fix ? 'Waiting for your GPS fix' : 'Their GPS has no fix'}</div>
        <div class="peer-meta">
          <span>${bars(p.rssi)} ${p.rssi} dBm · SNR ${p.snr}</span>
          <span>Battery ${p.bat >= 0 ? `${p.bat}%` : '—'}</span>
          <span>${p.sats} sats${p.hdop < 25 ? ` · HDOP ${p.hdop}` : ''}</span>
          <span>${fmtAge(age)}</span>
          ${leash ? '<span class="tag warn">beyond leash</span>' : ''}
        </div>
        <div class="peer-actions">
          <button class="btn small ghost" data-act="map" ${p.fix ? '' : 'disabled'}>Show on map</button>
          <button class="btn small ghost" data-act="scan" ${link.state === 'on' ? '' : 'disabled'}>Scan around</button>
        </div>
      </div></div>`;
  }).join('');
  updateArrows();
}

function updateArrows() {
  for (const card of $('peerList').querySelectorAll('.peer-card')) {
    const b = parseFloat(card.dataset.bearing);
    if (Number.isNaN(b)) continue;
    const rot = state.heading == null ? b : b - state.heading;
    card.querySelector('.arrow-dial svg').style.transform = `rotate(${rot}deg)`;
    if (state.heading != null) {
      const sub = card.querySelector('.turn');
      sub.textContent = `${compass(b)} · ${Math.round(b)}° · ${relTurn(b)}`;
    }
  }
}

$('peerList').addEventListener('click', (e) => {
  const btn = e.target.closest('button[data-act]');
  if (!btn) return;
  const id = btn.closest('.peer-card').dataset.id;
  const p = state.peers.get(id);
  if (btn.dataset.act === 'map' && p) {
    showTab('map');
    setFollow(false);
    map.focus(p.lat, p.lon);
  } else if (btn.dataset.act === 'scan') {
    requestScan(id);
    showTab('tools');
  }
});

setInterval(() => {
  if (activeTab === 'peers') renderPeers();
  for (const p of state.peers.values()) map.setPeer(p, { stale: Date.now() - p.heardAt > 30000, sos: state.sosFrom === p.id });
}, 2000);

// ---------------------------------------------------------------- sky

function renderSkyTab() {
  renderGpsStats($('gpsStats'), state.self);
  renderSky($('skyPlot'), $('skyLegend'), $('snrBars'), state.sky);
}

// ---------------------------------------------------------------- chat

function loadChat() {
  state.msgs = new Map(store(`chat:${state.boardId}`) || []);
  renderChat();
}

function saveChat() {
  if (!state.boardId) return;
  const entries = [...state.msgs.entries()].sort((a, b) => a[1].at - b[1].at).slice(-100);
  store(`chat:${state.boardId}`, entries);
}

function renderChat() {
  const log = $('chatLog');
  const msgs = [...state.msgs.values()].sort((a, b) => a.at - b.at);
  if (!msgs.length) return;
  const stickToBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 40;
  log.innerHTML = msgs.map((m) => {
    const status = m.me ? { wait: '· sending', ok: '· ✓ delivered', fail: '· not delivered' }[m.st] ?? '' : '';
    const time = new Date(m.at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
    return `<div class="bubble ${m.me ? 'mine' : ''} ${m.sos ? 'sos' : ''}">
      <div class="who">${m.me ? 'You' : esc(m.from)}</div><div class="txt">${esc(m.text)}</div>
      <div class="meta">${time} ${status}</div></div>`;
  }).join('');
  if (stickToBottom) log.scrollTop = log.scrollHeight;
}

function updateBadge() {
  const b = $('chatBadge');
  b.hidden = !state.unread;
  b.textContent = state.unread;
}

$('chatText').addEventListener('input', (e) => {
  // what the boards can show: printable ASCII, uppercase
  const clean = e.target.value.toUpperCase().replace(/[^\x20-\x7E]/g, '');
  if (clean !== e.target.value) e.target.value = clean;
  $('chatCount').textContent = `${clean.length}/32`;
});

$('chatForm').addEventListener('submit', (e) => {
  e.preventDefault();
  const text = $('chatText').value.trim();
  if (!text) return;
  link.send({ c: 'send', text });
  $('chatText').value = '';
  $('chatCount').textContent = '0/32';
});

// ---------------------------------------------------------------- SOS

$('sosBtn').addEventListener('click', () => {
  if (confirm('Send SOS to every board in range?')) link.send({ c: 'sos' });
});
$('sosAckBtn').addEventListener('click', () => {
  $('sosModal').hidden = true;
  link.send({ c: 'sosack' });
  const p = state.peers.get(state.sosFrom);
  state.sosFrom = null;
  if (p) map.setPeer(p, { stale: false, sos: false });
});
$('sosShowBtn').addEventListener('click', () => {
  const p = state.peers.get(state.sosFrom);
  $('sosModal').hidden = true;
  showTab('map');
  if (p?.fix) map.focus(p.lat, p.lon);
});

// ---------------------------------------------------------------- scan

function updateScanTargets() {
  const sel = $('scanTarget');
  const have = new Set([...sel.options].map((o) => o.value));
  for (const id of state.peers.keys())
    if (!have.has(id)) sel.add(new Option(`Board ${id}`, id));
}

function requestScan(target) {
  const what = ($('scanWifi').checked ? 1 : 0) | ($('scanBle').checked ? 2 : 0);
  if (!what) return toast('Pick Wi-Fi and/or BLE');
  $('scanTarget').value = target;
  link.send({ c: 'scan', id: target, what });
  scans.pending(target);
}

$('scanBtn').addEventListener('click', () => requestScan($('scanTarget').value));

// ---------------------------------------------------------------- range test

function renderRange() {
  const s = range.summary();
  $('rangeBtn').textContent = range.running ? 'Stop' : 'Start';
  $('rangeStats').innerHTML = [['Heard', s.heard], ['Missed', s.missed], ['Farthest', s.far], ['Weakest', s.weakest]]
    .map(([k, v]) => `<div class="stat"><div class="k">${k}</div><div class="v">${v}</div></div>`).join('');
}

$('rangeBtn').addEventListener('click', () => {
  const on = range.toggle();
  if (on && !state.self?.fix) toast('Waiting for a GPS fix on your board before logging.');
  renderRange();
});
$('rangeShowBtn').addEventListener('click', () => {
  $('layerRange').checked = true;
  map.showLayer('range', true);
  map.setRangeSamples(range.samples);
  showTab('map');
});
$('rangeCsvBtn').addEventListener('click', () => range.exportCsv());
$('rangeClearBtn').addEventListener('click', () => {
  if (!confirm('Delete all range-test samples?')) return;
  range.clear();
  map.setRangeSamples([]);
  renderRange();
});

// ---------------------------------------------------------------- waypoints

function renderWaypoints() {
  const self = state.self;
  $('wpList').innerHTML = state.wps.length
    ? state.wps.map(([name, lat, lon], i) => {
        const where = self?.fix ? `${fmtDist(distM(self, { lat, lon }))} ${compass(bearing(self, { lat, lon }))}` : '';
        return `<div class="wp-row" data-i="${i}"><span class="name">${esc(name)}</span><span class="d">${where}</span>
          <button class="btn small ghost" data-act="map">Map</button>
          <button class="btn small ghost" data-act="ren">Rename</button>
          <button class="btn small ghost danger" data-act="del">Delete</button></div>`;
      }).join('')
    : '<p class="muted">No waypoints. Key MARK in Morse on a board, or use "Mark here".</p>';
}

$('wpList').addEventListener('click', (e) => {
  const btn = e.target.closest('button[data-act]');
  if (!btn) return;
  const i = +btn.closest('.wp-row').dataset.i;
  const [name, lat, lon] = state.wps[i];
  if (btn.dataset.act === 'map') {
    showTab('map');
    map.focus(lat, lon);
  } else if (btn.dataset.act === 'ren') {
    const n = prompt('New name', name)?.trim().toUpperCase().slice(0, 11);
    if (n) link.send({ c: 'renmark', i, name: n });
  } else if (btn.dataset.act === 'del' && confirm(`Delete waypoint ${name}?`)) link.send({ c: 'delmark', i });
});

$('markBtn').addEventListener('click', () => {
  link.send({ c: 'mark', name: $('markName').value.trim().toUpperCase().slice(0, 11) });
  $('markName').value = '';
});

// ---------------------------------------------------------------- GPX

$('gpxBtn').addEventListener('click', () => {
  state.track = { n: 0, pts: [] };
  $('gpxProgress').hidden = false;
  $('gpxProgress').value = 0;
  link.send({ c: 'gpx' });
});

$('gpxClearBtn').addEventListener('click', () => {
  if (confirm("Delete the track stored on the board? Download it first if you want to keep it.")) link.send({ c: 'gpxclear' });
});

function onTrack(m) {
  if (!state.track) return;
  if (m.t === 'trk') state.track.n = m.n;
  else if (m.t === 'trkp') {
    state.track.pts.push(...m.p);
    $('gpxProgress').value = state.track.n ? state.track.pts.length / state.track.n : 0;
  } else {
    const pts = state.track.pts;
    state.track = null;
    $('gpxProgress').hidden = true;
    if (!pts.length) return toast('The board has no track points yet.');
    download(`buddy-${state.boardId}-${new Date().toISOString().slice(0, 10)}.gpx`, buildGpx(pts), 'application/gpx+xml');
  }
}

function buildGpx(pts) {
  const wpts = state.wps.map(([name, lat, lon]) => `  <wpt lat="${lat}" lon="${lon}"><name>${esc(name)}</name></wpt>`).join('\n');
  let segs = '', prevT = 0;
  for (const [lat, lon, ele, t] of pts) {
    if (prevT && t - prevT > 300) segs += '    </trkseg>\n    <trkseg>\n'; // >5 min gap: new segment
    segs += `      <trkpt lat="${lat}" lon="${lon}"><ele>${ele}</ele><time>${new Date(t * 1000).toISOString()}</time></trkpt>\n`;
    prevT = t;
  }
  return `<?xml version="1.0" encoding="UTF-8"?>
<gpx version="1.1" creator="Buddy Tracker" xmlns="http://www.topografix.com/GPX/1/1">
${wpts}
  <trk><name>Buddy ${esc(state.boardId)}</name>
    <trkseg>
${segs}    </trkseg>
  </trk>
</gpx>
`;
}

// ---------------------------------------------------------------- settings

function renderSettings() {
  const c = state.cfg;
  if (!c) return;
  $('trackInfo').textContent = `The board logs a point every 5 s while moving. ${c.trk} points stored.`;
  if ($('settingsForm').contains(document.activeElement)) return; // don't clobber edits in progress
  $('setLeashM').value = c.leashM;
  $('setLeashMin').value = c.leashMin;
  $('setScreenS').value = c.screenS;
  $('setNorthRight').value = String(c.northRight);
}

$('settingsForm').addEventListener('submit', (e) => {
  e.preventDefault();
  link.send({
    c: 'set',
    leashM: +$('setLeashM').value || 0,
    leashMin: +$('setLeashMin').value || 0,
    screenS: +$('setScreenS').value || 0,
    northRight: +$('setNorthRight').value,
  });
  document.activeElement?.blur();
  toast('Saved to board');
});

$('forgetBtn').addEventListener('click', () => {
  if (confirm('Forget every paired phone on this board? You will need to pair again (and remove the board from Android Bluetooth settings).'))
    link.send({ c: 'forget' });
});

// ---------------------------------------------------------------- feedback

function toast(text, warn = false) {
  const t = document.createElement('div');
  t.className = `toast ${warn ? 'warn' : ''}`;
  t.textContent = text;
  $('toasts').append(t);
  setTimeout(() => t.remove(), warn ? 6000 : 3500);
}

let audio;
function beep(count) {
  try {
    audio ||= new AudioContext();
    for (let i = 0; i < count; i++) {
      const o = audio.createOscillator(), g = audio.createGain();
      const t = audio.currentTime + i * 0.35;
      o.frequency.value = 880;
      g.gain.setValueAtTime(0.25, t);
      g.gain.exponentialRampToValueAtTime(0.001, t + 0.25);
      o.connect(g).connect(audio.destination);
      o.start(t);
      o.stop(t + 0.25);
    }
  } catch {
    /* audio blocked until the user interacts; vibration still works */
  }
}

renderRange();

window.buddy = { link, state }; // handy from the devtools console
if (new URLSearchParams(location.search).has('demo')) import('./demo.js');
