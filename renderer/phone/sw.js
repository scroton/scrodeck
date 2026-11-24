'use strict';
// The service worker exists for two reasons: Android will not offer a real app install (a WebAPK,
// with its own icon and no browser chrome) without one, and it keeps the shell openable when the PC
// is asleep — the page loads and says it is reconnecting instead of showing a browser error.
//
// Network first, always: this page is a live view of the deck, so a stale cached copy would be worse
// than a slow one. The cache is only the fallback when the PC cannot be reached.
const CACHE = 'deck-shell-v144';
const SHELL = ['/', '/style.css?v=144', '/app.js?v=144', '/manifest.webmanifest', '/icon-192.png', '/icon-512.png', '/vendor/xterm.js', '/vendor/xterm.css', '/vendor/addon-fit.js'];

self.addEventListener('install', e => {
  e.waitUntil(caches.open(CACHE).then(c => c.addAll(SHELL)).then(() => self.skipWaiting()).catch(() => self.skipWaiting()));
});
self.addEventListener('activate', e => {
  // Take over, drop the old caches, and reload whatever is already open. Installed to the home
  // screen there is no address bar, so without this a client that once fell back to a cached shell
  // (the PC was briefly down) would keep serving that shell until someone cold-started it twice.
  e.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
      .then(() => self.clients.claim())
      .then(() => self.clients.matchAll({ type: 'window' }))
      .then(cs => cs.forEach(c => { try { c.navigate(c.url); } catch { /* not navigable */ } }))
  );
});
self.addEventListener('fetch', e => {
  const req = e.request;
  if (req.method !== 'GET' || new URL(req.url).origin !== location.origin) return;
  e.respondWith(
    // the app's own files are fetched past any HTTP cache: a redeploy must win at once
    fetch(/\/(index\.html|app\.js|style\.css|sw\.js)|\/(\?|$)/.test(new URL(req.url).pathname + new URL(req.url).search) ? new Request(req, { cache: 'no-cache' }) : req)
      .then(res => {
        // keep the shell fresh for the next cold start; never cache an error page
        if (res.ok) { const copy = res.clone(); caches.open(CACHE).then(c => c.put(req, copy)).catch(() => {}); }
        return res;
      })
      .catch(() => caches.match(req).then(hit => hit || caches.match('/')))
  );
});
