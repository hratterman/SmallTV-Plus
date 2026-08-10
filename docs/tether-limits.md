# What the tether can and cannot carry

The tether is a serial cable to a browser tab: the cube frames a request, the page
calls `fetch()`, the bytes come back. That makes two things decide whether a
feature works over the cable, and neither is obvious from the feature's code.

1. **CORS.** The page is JavaScript on some origin. It may only *read* a
   cross-origin response the server marks readable. A server that sends no
   `access-control-allow-origin` is unreachable from the cable no matter how
   willing the cube is — the request goes out and the browser refuses to hand
   over the answer.
2. **Shape.** `fetch()` is one request, one response. Anything needing a socket
   the server pushes down is not merely slow over the cable, it is impossible.

Everything below was measured with `curl -H "Origin: https://example.com"`, not
inferred from documentation. Re-run it before trusting it; these headers change.

## Measured

| Host | Header | Verdict |
|---|---|---|
| `accounts.spotify.com`, `api.spotify.com` | reflects origin | works |
| `i.scdn.co` (album art) | `*` | works |
| `stockanalysis.com` — quote *and* `/history` | `*` | works |
| `raw.githubusercontent.com` | `*` | works |
| `api.coingecko.com` | `*` | works (crypto only) |
| **`www.googleapis.com/calendar/v3`** | **reflects origin** | **works** |
| **`oauth2.googleapis.com/token`** | **reflects origin** | **works** |
| **`graph.microsoft.com/v1.0`** | **`*`** | **works** |
| **`login.microsoftonline.com`** | **`*`** | **works** |
| `query1/2.finance.yahoo.com` | none, on 200 and on 429 | blocked |
| `calendar.google.com/.../basic.ics` | none | blocked |
| `opendata.adsb.fi` (radar) | none | blocked |
| Stooq | none | blocked |

## Calendar: use the API, not the ICS feed

The obvious design is the secret iCal URL both Google and Outlook publish per
calendar — no OAuth, no helper, just a GET. It is the wrong one here: those
endpoints send no CORS header, so the feed works on WiFi and dies on the cable.

The REST APIs are the opposite, and deliberately so — Google and Microsoft both
ship browser JavaScript clients, so their APIs *have* to be readable
cross-origin, and their token endpoints with them. That means the whole OAuth
refresh-token dance runs over the tether unchanged.

So calendar follows Spotify's existing pattern rather than inventing one: obtain
a refresh token once with a helper on a real computer, store it on the cube, and
let the cube trade it for access tokens as needed. Every request involved is a
plain HTTPS call to a CORS-clean host.

## Mining: no, and not for want of looking

Stratum is a long-lived raw TCP socket carrying server-pushed jobs. A browser
cannot open one — the web platform exposes no such API, deliberately. Ruled out
in turn:

- **`fetch()`** — one request, one response. There is no request that causes a
  `mining.notify` to arrive, because the pool sends those on its own schedule.
- **WebSocket** — genuinely promising, because WebSocket is *not* CORS-restricted
  and a page may open one to any host. It fails on the other side: no Bitcoin
  pool speaks stratum over WebSocket. Every browser-mining stack that appears to
  (coin-hive-stratum, coinhive-stratum-mining-proxy, xmrwasp) is a proxy the user
  runs, translating WSS to TCP. The `wss://` endpoint belongs to the proxy.
- **Direct Sockets** — real, and does give raw TCP, but only to Isolated Web
  Apps: an installed app under enterprise policy, not a page you open.
- **getblocktemplate over HTTP** — solo mining against your own `bitcoind`,
  which is a node to run, not a pool to point at.

Every remaining route needs a program on the host, which is the thing the tether
exists to avoid.

**The answer for an office is the laptop's built-in hotspot**, not the cable.
It is an OS toggle rather than software to install, the cube joins it as ordinary
WiFi, and then *everything* works — mining, Yahoo, radar, the lot — because there
is no browser in the path. The cable remains the fallback for when a machine
forbids the hotspot.
