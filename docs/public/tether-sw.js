// Service worker for the SmallTV tether page. Two jobs only:
//  - make the page installable as its own app window, and
//  - keep a copy cached so it opens even with the network down (the tether
//    itself needs no internet to configure a cube - only the proxying does).
//
// The page is fetched network-first so an updated deployment reaches everyone
// on their next open; the cache is the fallback, not the source of truth.
const CACHE = 'tether-v1';
const SHELL = ['./tether.html', './tether.webmanifest',
               './tether-icon-192.png', './tether-icon-512.png'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', e => {
  e.waitUntil((async () => {
    for (const k of await caches.keys()) if (k !== CACHE) await caches.delete(k);
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', e => {
  const url = new URL(e.request.url);
  if (url.origin !== location.origin) return;   // never intercept the cube's proxied traffic
  e.respondWith((async () => {
    try {
      const fresh = await fetch(e.request);
      const c = await caches.open(CACHE);
      c.put(e.request, fresh.clone());
      return fresh;
    } catch (_) {
      const hit = await caches.match(e.request, { ignoreSearch: true });
      if (hit) return hit;
      throw _;
    }
  })());
});
