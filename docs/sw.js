// Offline support: the app shell is cached (stale-while-revalidate, so a new
// deploy shows up on the next launch), and map tiles you've looked at are kept
// so the map still works without a data connection.

const SHELL = 'buddy-shell-v1';
const TILES = 'buddy-tiles-v1';
const MAX_TILES = 2000;

const SHELL_FILES = [
  './', 'index.html', 'style.css', 'app.js', 'ble.js', 'map.js', 'sky.js', 'scan.js', 'range.js', 'util.js', 'demo.js',
  'manifest.webmanifest', 'icons/icon-192.png', 'icons/icon-512.png',
  'https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.css',
  'https://cdnjs.cloudflare.com/ajax/libs/leaflet/1.9.4/leaflet.min.js',
];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(SHELL).then((c) => c.addAll(SHELL_FILES)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', (e) => {
  e.waitUntil(
    caches.keys()
      .then((keys) => Promise.all(keys.filter((k) => ![SHELL, TILES].includes(k)).map((k) => caches.delete(k))))
      .then(() => self.clients.claim()),
  );
});

self.addEventListener('fetch', (e) => {
  const req = e.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);

  if (url.hostname.endsWith('tile.openstreetmap.org')) {
    e.respondWith(tile(req));
  } else if (req.mode === 'navigate') {
    // any page URL (?demo, ?tab=...) is the same app
    e.respondWith(staleWhileRevalidate(new Request(new URL('./', self.registration.scope))));
  } else if (url.origin === location.origin || url.hostname === 'cdnjs.cloudflare.com') {
    e.respondWith(staleWhileRevalidate(req));
  }
});

async function staleWhileRevalidate(req) {
  const cache = await caches.open(SHELL);
  const cached = await cache.match(req, { ignoreSearch: true });
  const network = fetch(req)
    .then((res) => {
      if (res.ok) cache.put(req, res.clone());
      return res;
    })
    .catch(() => cached);
  return cached || network;
}

async function tile(req) {
  const cache = await caches.open(TILES);
  const cached = await cache.match(req);
  if (cached) return cached;
  try {
    const res = await fetch(req);
    if (res.ok) {
      await cache.put(req, res.clone());
      trimTiles(cache);
    }
    return res;
  } catch {
    return new Response('', { status: 504 });
  }
}

async function trimTiles(cache) {
  const keys = await cache.keys();
  for (let i = 0; i < keys.length - MAX_TILES; i++) await cache.delete(keys[i]); // oldest first
}
