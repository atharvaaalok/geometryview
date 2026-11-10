// Makes a page served without the headers threads need, as GitHub Pages serves it, cross-origin
// isolated: the page registers this worker, which adds COOP, COEP and CORP to each response from
// the page's own origin. Served with the headers already, by the launcher, the page never
// registers it. Everything the page loads is its own, so require-corp blocks nothing.

self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", (event) => event.waitUntil(self.clients.claim()));

self.addEventListener("fetch", (event) => {
  const request = event.request;
  if (new URL(request.url).origin !== self.location.origin) return;
  if (request.cache === "only-if-cached" && request.mode !== "same-origin") return;
  event.respondWith(fetch(request).then((response) => {
    if (response.status === 0 || response.type === "opaqueredirect") return response;
    const headers = new Headers(response.headers);
    headers.set("Cross-Origin-Opener-Policy", "same-origin");
    headers.set("Cross-Origin-Embedder-Policy", "require-corp");
    headers.set("Cross-Origin-Resource-Policy", "same-origin");
    return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
  }));
});
