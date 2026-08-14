// config.h — compile-time constants for smalltv-plus
//
// Hardware: three board variants, all a 1.54" 240x240 ST7789 IPS panel:
//   - Original GeekMagic SmallTV: ESP-12F (ESP8266)      [board_esp8266.h]
//   - Knockoff SmallTV:           ESP32-C2 / ESP8684      [board_esp32c2.h]
//   - NMMiner NM-TV-154:          classic ESP32 (WROOM-32E) [board_esp32.h]
// The board-specific pin map + panel quirks live in the board headers, selected
// below by the build-time target macro. Everything else here is shared.
#pragma once

// ---------------------------------------------------------------------------
// Firmware identity
// ---------------------------------------------------------------------------
#define FW_NAME     "smalltv-plus"
#define FW_VERSION  "2.9.6"

// Project / update references (shown in the web UI; used by the GitHub self-update)
#define REPO_URL      "https://github.com/hratterman/smalltv-plus"
#define REPO_OWNER    "hratterman"
#define REPO_NAME     "smalltv-plus"
// Release asset the GitHub self-updater pulls — one app image per target.
#if defined(SMALLTV_ESP32C2)
  #define UPDATE_ASSET "smalltv-plus-firmware-c2.bin"
#elif defined(SMALLTV_ESP32_PRO)
  #define UPDATE_ASSET "smalltv-plus-firmware-esp32-pro.bin"
#elif defined(SMALLTV_ESP32)
  #define UPDATE_ASSET "smalltv-plus-firmware-esp32.bin"
#else
  #define UPDATE_ASSET "smalltv-plus-firmware.bin"
#endif
#define GH_API_HOST   "api.github.com"
#define DAEMON_URL    "https://github.com/giovi321/clawdmeter-daemon"

// ---------------------------------------------------------------------------
// Display wiring + panel quirks — board-specific, pulled from the right header.
// Provides TFT_SCLK/MOSI/DC/RST/CS/BL, TFT_BGR, TFT_BL_DEFAULT_INVERTED,
// HAS_LDR/LDR_PIN/ADC_MAX. Both panels are 1.54" 240x240 ST7789 IPS.
// ---------------------------------------------------------------------------
#if defined(SMALLTV_ESP32C2)
  #include "board_esp32c2.h"
#elif defined(SMALLTV_ESP32_PRO)
  #include "board_esp32_pro.h"
#elif defined(SMALLTV_ESP32)
  #include "board_esp32.h"
#else
  #include "board_esp8266.h"
#endif

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// ---------------------------------------------------------------------------
// Limits (bound RAM usage on the ESP8266)
// ---------------------------------------------------------------------------
#define MAX_SYMBOLS       8    // max tickers in the rotation
#define MAX_SYMBOL_LEN   24    // e.g. "BTC-USD", cash.ch key "147478611-246-333"
#define MAX_WIFI_NETS     4    // saved WiFi networks; strongest visible wins at boot
#define MAX_NAME_LEN     20    // friendly name shown on screen
#define MAX_SPARK_POINTS 60    // sparkline samples kept per symbol
#define MAX_URL_LEN     200    // webhook base URL

// ---------------------------------------------------------------------------
// Display mode — what the device shows
//   0 = stock / crypto ticker (per-symbol source, see SRC_* below)
//   1 = Claude usage meter (mascot + 5h/7d usage bars, fed by the daemon/)
//   2 = plane radar
//   3 = carousel: rotate through the ticked features on a timer
//   4 = bitcoin solo miner (ESP32 targets only, see WITH_MINER below)
//   5 = clock (SNTP wall time, big digits)
//   6 = spotify now-playing (ESP32 targets only)
//   7 = ambient patterns (no network, nothing to report)
//   8 = blackjack, played with the lid pad
//   9 = calendar: the next obligation, from Google or Outlook
// ---------------------------------------------------------------------------
#define MODE_STOCKS    0
#define MODE_USAGE     1
#define MODE_RADAR     2
#define MODE_CAROUSEL  3
#define MODE_MINER     4
#define MODE_CLOCK     5
#define MODE_SPOTIFY   6
#define MODE_AMBIENT   7
#define MODE_BLACKJACK 8
#define MODE_CALENDAR  9
#define MODE_WEATHER   10
#define DEFAULT_MODE MODE_STOCKS
#define DEFAULT_CAROUSEL_SEC 30      // per-mode dwell in carousel

// The ambient patterns, and which of them the user has ticked. Here rather than
// in AmbientMode.h because Settings.cpp needs them too, and AmbientMode.h
// cannot be included from there without a cycle through Mode.h.
#define AMB_PAT_LIFE      0
#define AMB_PAT_PLASMA    1
#define AMB_PAT_STARS     2
#define AMB_PAT_RAIN      3
#define AMB_PAT_SPARKS    4
#define AMB_PATTERNS      5
#define AMB_PATTERN_ALL   ((uint8_t)((1u << AMB_PATTERNS) - 1))

// ---------------------------------------------------------------------------
// Compile-time feature toggles. All shipping features are on by default; a lean
// build drops one by setting e.g. -D WITH_RADAR=0 in a PlatformIO env, which
// omits that feature's module from the registry and its web UI section.
// (WITH_RADAR ships off until the radar module lands.)
// ---------------------------------------------------------------------------
#ifndef WITH_TICKER
#define WITH_TICKER 1
#endif
#ifndef WITH_USAGE
#define WITH_USAGE 1
#endif
#ifndef WITH_RADAR
#define WITH_RADAR 1
#endif
#ifndef WITH_CLOCK
#define WITH_CLOCK 1
#endif
// Spotify needs TLS, a few KB of JSON parsing and the flash to hold both, so it
// is ESP32-only like the miner.
#ifndef WITH_SPOTIFY
#if defined(SMALLTV_ESP32)
#define WITH_SPOTIFY 1
#else
#define WITH_SPOTIFY 0
#endif
#endif

#define DEFAULT_SPOTIFY_POLL_SEC 10

// Calendar needs the same TLS + JSON stack as Spotify, so it lives on the same
// targets. Both providers are readable from a browser (their token endpoints
// included — measured, see docs/tether-limits.md), so it works over the tether.
#ifndef WITH_CALENDAR
#if defined(SMALLTV_ESP32)
#define WITH_CALENDAR 1
#else
#define WITH_CALENDAR 0
#endif
#endif

// Weather: Open-Meteo, no key, no account, and Access-Control-Allow-Origin: *
// (measured) — so it works identically on WiFi and over the tether cable.
#ifndef WITH_WEATHER
#if defined(SMALLTV_ESP32)
#define WITH_WEATHER 1
#else
#define WITH_WEATHER 0
#endif
#endif
// The device path connects to the RESOLVED ADDRESS and routes by Host header
// (Open-Meteo accepts that with HTTP/1.1 - measured), so the TLS hello's SNI
// carries only an IP literal: a hotspot middlebox filtering handshakes by
// hostname has nothing to match. If even that TLS is killed, the client
// falls back to plain http (measured clean on the field hotspot; public
// data). The tether path stays a normal https URL - the browser speaks
// TLS 1.3 through the same filters.
#define WEATHER_HOST "api.open-meteo.com"
#define WEATHER_PATH "/v1/forecast"

// The device-wide numerals style: how the ticker price, the usage meters and
// the miner hashrate draw their big numbers. Letters always stay in the pixel
// font — the sans faces carry only 0x20..0x3A (see tools/gen_font.py).
#define NUM_FONT_PIXEL 0
#define NUM_FONT_SANS  1
// How many sans sizes exist (big/mid/small). Mirrored from the generated
// NumFonts.h so call sites can clamp a face without pulling the font data in;
// Gfx.cpp static_asserts the two stay equal.
#define NUM_FACES 3

// Clock faces. The sans face is a rasterised LiberationSans-Bold subset
// (tools/gen_font.py); the seven-segment face is drawn from rectangles.
#define CLOCK_FACE_PIXEL  0
#define CLOCK_FACE_SANS   1
#define CLOCK_FACE_7SEG   2
#define CLOCK_FACE_ANALOG 3   // dial and hands; seconds stay on the bar below
#define CLOCK_FACE_FLIP   4   // split-flip cards with the big sans digits

#define DEFAULT_CALENDAR_POLL_SEC 300   // meetings do not move that fast
#define CAL_MAX_EVENTS 6                // shown: 1 big + 3 small; 2 spare for filtering
#define CAL_TITLE_LEN  64
#define CAL_LOOKAHEAD_SEC (48UL * 3600UL)  // two days is "next obligation" territory
#define CAL_GOOGLE    0
#define CAL_MICROSOFT 1
#define CAL_ICS       2   // secret iCal URL: no OAuth, no registration, WiFi only
#define GOOGLE_TOKEN_URL "https://oauth2.googleapis.com/token"
#define GOOGLE_CAL_URL   "https://www.googleapis.com/calendar/v3/calendars/primary/events"
#define MS_TOKEN_URL     "https://login.microsoftonline.com/common/oauth2/v2.0/token"
#define MS_DEVICE_URL    "https://login.microsoftonline.com/common/oauth2/v2.0/devicecode"
#define MS_CAL_URL       "https://graph.microsoft.com/v1.0/me/calendarview"

// Largest album cover accepted when it has to be held whole (the tether route).
// Spotify's 300 px JPEGs run 15-25 KB; this leaves room for a dense one without
// letting a surprise take the heap.
#define ART_MAX_BYTES 40960

// Ambient patterns. Only the grid buffers cost anything (~7 KB) and there is no
// network path at all, so this is on everywhere the display is.
#ifndef WITH_AMBIENT
#define WITH_AMBIENT 1
#endif

// Ambient is the one mode meant to be watched rather than read, so it holds the
// carousel far longer than the informational screens and rotates its own
// patterns inside that block.
#define DEFAULT_AMBIENT_DWELL_SEC   180
#define DEFAULT_AMBIENT_PATTERN_SEC 45

// The game needs the pad, so it only exists where there is one to press.
#ifndef WITH_GAME
#if defined(SMALLTV_ESP32)
#define WITH_GAME 1
#else
#define WITH_GAME 0
#endif
#endif
// Miner is classic-ESP32 only: the mining core runs FreeRTOS worker tasks
// pinned across both cores and (optionally) the ESP32 SHA peripheral. The
// ESP8266 has neither, and the single-core C2 has no cycles to spare.
#ifndef WITH_MINER
#if defined(SMALLTV_ESP32)
#define WITH_MINER 1
#else
#define WITH_MINER 0
#endif
#endif

// Claude usage mode: once data stops arriving for this long (PC asleep, daemon
// stopped, network down) the screen switches from the stats to the idle mascot
// animation. Effective timeout also scales with the poll period (see main.cpp).
#define USAGE_STALE_GRACE_MS  20000UL

// ---------------------------------------------------------------------------
// Data source (stock mode)
//   0 = custom webhook (n8n / Node-RED / your own HTTP endpoint)
//   1 = Yahoo Finance, fetched directly by the device (no backend needed)
//   2 = cash.ch, fetched directly by the device (Swiss instruments, incl.
//       off-exchange structured products that Yahoo doesn't carry)
// ---------------------------------------------------------------------------
#define SRC_WEBHOOK  0
#define SRC_YAHOO    1
#define SRC_CASH     2
#define SRC_GHUB     3   // static JSON published to the repo's data branch (see below)
#define SRC_SA       4   // stockanalysis.com — the one quote source a browser may read
#define DEFAULT_SOURCE  SRC_YAHOO            // works out of the box, no server

// Yahoo Finance public chart endpoint. A browser-like User-Agent is required —
// requests with an empty UA are rejected with HTTP 429. TLS records from Yahoo
// are <=~1.3 KB, so the 4 KB BearSSL receive buffer in StockClient is plenty.
// query1/query2 are interchangeable mirrors; we fall back to the second on a
// transient failure (a single back-to-back HTTPS fetch occasionally drops).
#define YAHOO_CHART_HOST1 "query1.finance.yahoo.com"
#define YAHOO_CHART_HOST2 "query2.finance.yahoo.com"
#define YAHOO_CHART_PATH  "/v8/finance/chart/"
#define YAHOO_USER_AGENT  "Mozilla/5.0 (SmallTV)"

// cash.ch public GraphQL endpoint. The device sends two small hand-written
// GraphQL queries per symbol as plain GETs (?query=...): a ~200 B quote and a
// slim daily-close series for the sparkline. No API key, no cookies, no
// required headers. The symbol is the cash.ch listing key
// `valor-marketId-currencyId` (see the docs for how to find it).
// cash.ch's CDN requires ECDHE. The ESP32 targets (mbedTLS) do this easily. The
// ESP8266 (BearSSL) can too, but the handshake is memory-tight, so the cash
// path is shaped to fit: only cash.ch is offered ECDHE (Yahoo and the GitHub
// source are pinned to the cheap static-RSA suites), the connection uses 512 B
// buffers + TLS session resumption, and StockClient skips a fetch unless a
// large enough contiguous heap block is free. The GitHub source below is a
// zero-crash fallback if a device ever proves too tight for the direct path.

// GitHub source (SRC_GHUB): a scheduled workflow (.github/workflows/quotes.yml)
// fetches cash.ch server-side and publishes one JSON file per listing key to
// the repo's `data` branch. The device reads it from raw.githubusercontent.com,
// which — unlike cash.ch — still accepts the ESP8266's static-RSA handshake
// (the same one GitHub self-update and Yahoo use). The file is the same JSON
// the webhook parser accepts. The symbol is the cash.ch listing key; only keys
// listed in quotes-config.json are published. raw sends a ~4 KB certificate
// record and does not negotiate MFLN, so this path uses a larger TLS buffer.
#define GH_QUOTES_BASE "https://raw.githubusercontent.com/" REPO_OWNER "/" REPO_NAME "/data/quotes/"
#define GH_QUOTES_RXBUF 5120
// stockanalysis.com. The reason this source exists is narrow and worth stating:
// Yahoo sends no access-control-allow-origin on any response, so while the cube
// is tethered — fetching through a browser tab — Yahoo is unreachable however
// willing everyone is. This one answers with `*`, so it works over the cable and
// over WiFi alike, and the ticker falls back to it automatically when tethered.
//
// It is an undocumented endpoint of somebody's website rather than a published
// API. US stocks and ETFs only; no crypto, no foreign listings. Treated
// accordingly: a failure here is reported, never fatal.
#define SA_QUOTE_URL "https://stockanalysis.com/api/quotes/s/"
#define SA_HIST_URL  "https://stockanalysis.com/api/symbol/s/"
// Daily bars, so ask for just enough of them: 3M is 64 rows in ~6 KB, against
// MAX_SPARK_POINTS of 60. A year is 252 rows in ~25 KB, which overran the read
// buffer and failed the parse with no hint of why — and 192 of those rows were
// then thrown away.
#define SA_HIST_TAIL "/history?range=3M"

#define CASH_GQL_HOST   "www.cash.ch"
#define CASH_GQL_PATH   "/_/api/graphql/prod"
#define CASH_USER_AGENT "Mozilla/5.0 (SmallTV)"

// ---------------------------------------------------------------------------
// Plane radar (MODE_RADAR)
//   Data source (radar's own selector, independent of the stock one):
//     0 = adsb.fi opendata, fetched directly by the device over HTTPS (no key)
//     1 = custom webhook (a LAN proxy that pre-filters — robust on the ESP8266)
// ---------------------------------------------------------------------------
#define RADAR_SRC_DIRECT   0
#define RADAR_SRC_WEBHOOK  1
#define DEFAULT_RADAR_SRC  RADAR_SRC_DIRECT

// adsb.fi free open-data endpoint (no API key; public rate limit ~1 req/s).
// Full path: /api/v3/lat/{lat}/lon/{lon}/dist/{nm}
#define ADSB_HOST        "opendata.adsb.fi"
#define ADSB_PATH        "/api/v3/lat/"
#define ADSB_USER_AGENT  "Mozilla/5.0 (SmallTV)"

// Bound RAM: nearest N aircraft kept/drawn, and a few home-area airports.
#define MAX_AIRCRAFT     24
#define MAX_AIRPORTS      6
#define MAX_ICAO_LEN      8      // ICAO ident + NUL (e.g. "LSZH")

// Defaults (lat/lon 0,0 is the "not set yet" sentinel -> shows a prompt).
#define DEFAULT_RADAR_LAT       0.0f
#define DEFAULT_RADAR_LON       0.0f
#define DEFAULT_RADAR_RANGE_KM  20
#define DEFAULT_RADAR_POLL_SEC  10     // >=3 keeps us under the 1 req/s limit

// ---------------------------------------------------------------------------
// Capacitive touch pad (ESP32 only — native touch sensing lives on specific
// GPIOs the other chips do not have). The pad on the NM-TV-154's lid is wired
// and readable, but which GPIO it lands on is undocumented, so the pin is a
// setting the web UI can discover by watching every candidate while you tap.
// ---------------------------------------------------------------------------
#ifndef HAS_TOUCH
#if defined(SMALLTV_ESP32)
#define HAS_TOUCH 1
#else
#define HAS_TOUCH 0
#endif
#endif

#define DEFAULT_TOUCH_GPIO      -1    // -1 = not found yet; run detection
#define DEFAULT_TOUCH_THRESHOLD 20    // baseline minus reading, in touch units

// ---------------------------------------------------------------------------
// Bitcoin miner (MODE_MINER): solo-mines against a stratum pool, mining core
// ported from BitMaker-hub/NerdMiner_v2. Mining needs a BTC address set in
// the web UI; until then the mode just shows a prompt.
// ---------------------------------------------------------------------------
#define DEFAULT_POOL_HOST  "solo.ckpool.org"
#define DEFAULT_POOL_PORT  3333

// Which SHA-256 implementation the hash workers use.
//   sw     — both workers run the optimized software double-hash (midstate
//            cached per job + 16-bit early exit). Verified against real block
//            data by tools/miner_selftest.
//   hybrid — worker 0 drives the ESP32's hardware SHA peripheral while worker 1
//            stays on software. Only one worker can hold the engine, so this is
//            the fastest available arrangement.
//
// Measured on the NM-TV-154 (ESP32-D0WD-V3 @ 240 MHz): hardware ~350 KH/s,
// software ~50 KH/s for both cores together — a 7x gap, so hybrid is the
// default. Software stays available as the fallback the hardware path
// self-checks against, and is the only path on chips without the peripheral.
//
// For context on where the headroom is: the engine compresses a 64-byte block
// in ~72 cycles and a nonce costs 3 compressions (the classic ESP32's engine
// cannot be seeded with a midstate), so the ceiling is ~1.1 MH/s — which is
// where the stock NMMiner firmware's 1043 KH/s came from. Everything between
// that and what we measure is overhead in the register-driving loop.
#define MINER_ENGINE_SW      0
#define MINER_ENGINE_HYBRID  1
#define DEFAULT_MINER_ENGINE MINER_ENGINE_HYBRID

// ---------------------------------------------------------------------------
// USB tether: borrow a nearby computer's connection for networks the cube
// cannot join on its own. Discovered rather than configured — the cube
// announces itself and uses the cable if something answers.
// ---------------------------------------------------------------------------
#ifndef WITH_TETHER
#if defined(SMALLTV_ESP32)
#define WITH_TETHER 1
#else
#define WITH_TETHER 0
#endif
#endif

#define TETHER_HELLO_DOWN_MS  2000    // announcing, nobody listening yet
#define TETHER_HELLO_UP_MS   10000    // heartbeat once a host has answered
#define TETHER_IDLE_MS       25000    // silence this long = the cable is gone
#define TETHER_CFG_CHUNK       480    // settings JSON, bytes per frame
#define TETHER_CFG_MAX        8192    // largest settings document accepted

// ---------------------------------------------------------------------------
// Captive portal handling. Associating is not the same as being online, and on
// a hotel or conference network the difference is every feature silently
// failing while the screen shows a network name and an IP.
// ---------------------------------------------------------------------------
#ifndef WITH_CAPTIVE
#define WITH_CAPTIVE 1
#endif

// Plain HTTP on purpose: a portal cannot intercept HTTPS without the client
// noticing, so over TLS it just hangs, which looks the same as a dead network.
// Over HTTP it has to answer, and its answer is the detection.
#define CAPTIVE_PROBE_URL     "http://connectivitycheck.gstatic.com/generate_204"
#define CAPTIVE_TIMEOUT_MS    6000
#define CAPTIVE_RETRY_MS      30000UL    // while stuck behind a portal
#define CAPTIVE_OK_RECHECK_MS 300000UL   // confirm the route is still there
#define CAPTIVE_MAX_ATTEMPTS  3          // accept tries before giving up
#define CAPTIVE_MAX_PAGE      6144       // bytes of portal HTML to read
#define CAPTIVE_MAX_FIELDS    24         // form inputs to carry across

// ---------------------------------------------------------------------------
// Defaults (used on first boot / factory reset)
// ---------------------------------------------------------------------------
#define DEFAULT_AP_SSID      "SmallTV-Setup"
#define DEFAULT_AP_PASS      ""              // empty => open AP
#define DEFAULT_HOSTNAME     "smalltv"
#define DEFAULT_POLL_SEC      120            // how often to refresh data
#define TICKER_RETRY_SEC       12            // fast retry after a failed/skipped fetch
#define TICKER_RETRY_MAX        4            // consecutive fast retries before backing off
#define DEFAULT_ROTATE_SEC    10             // how long each symbol is shown
#define DEFAULT_RANGE        "1d"            // chart timeframe (e.g. 1d/5d/1mo/1y)
#define DEFAULT_POINTS        48             // sparkline points requested
#define DEFAULT_BRIGHTNESS    90             // 0..100 %
#define DEFAULT_HTTP_TIMEOUT  8000           // ms per request

// --- Clock / night mode (device-wide) ---
#define NTP_SERVER1             "pool.ntp.org"
#define NTP_SERVER2             "time.nist.gov"
#define DEFAULT_TZ_NAME         ""        // IANA display name; empty = UTC
#define DEFAULT_TZ_POSIX        "UTC0"    // POSIX TZ rule the device feeds SNTP
#define DEFAULT_NIGHT_ENABLED   false
#define DEFAULT_NIGHT_START_MIN 1320      // 22:00
#define DEFAULT_NIGHT_END_MIN   420       // 07:00
#define DEFAULT_NIGHT_LEVEL     0         // 0..100, 0 = backlight fully off

// Night-mode NTP trust: only ENTER night mode when the clock was confirmed by a
// successful NTP sync within NIGHT_NTP_TRUST_MS (else we assume the clock may be
// wrong and keep the screen on). While inside the window but unconfirmed, re-arm
// SNTP every NIGHT_NTP_RESYNC_MS until a fresh sync lands or the window ends
// (morning). Once night mode has switched on, it stays on until the window ends.
#define NIGHT_NTP_TRUST_MS      300000UL  // 5 min: max age of the sync that unlocks night
#define NIGHT_NTP_RESYNC_MS      30000UL  // re-sync attempt cadence while held off
