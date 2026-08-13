#include "WeatherClient.h"
#if WITH_WEATHER

#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include "NetFetch.h"
#include "Platform.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#if WITH_SPOTIFY
#include "SpotifyClient.h"   // spotifyNetLock: one TLS operation at a time
#endif

// ---------------------------------------------------------------------------
// Shared state, guarded by s_lock. The task writes, the display loop reads.
//
// The fetch used to run on the loop thread out of WeatherMode::service, which
// froze the whole UI — touch sampling included — for the length of a TLS
// handshake whenever this mode came up with a stale reading. Tapping off the
// calendar page onto this one was the reproducible victim: the poll only
// advanced while the weather page was showing, so arriving here it was always
// overdue, the fetch ran before the first paint, and the tap looked dead —
// the calendar stayed on the glass and every further tap fell on a thread
// that was inside mbedTLS. Same cure as Spotify and the calendar: a task.
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_lock = nullptr;
static TaskHandle_t      s_task = nullptr;
static WeatherData       s_data;
static uint32_t          s_cfgFp = 0;   // loop-side only: change detector

// What the fetch needs from Settings, copied under the lock: the task must
// not read g_settings, which the loop rewrites on every web-UI save.
static struct {
  volatile uint32_t epoch;
  float    lat, lon;
  bool     unitsF;
  uint16_t pollSec;
} s_cfg;

static inline void lockTake() { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void lockGive() { if (s_lock) xSemaphoreGive(s_lock); }

void weatherSnapshot(WeatherData& out) {
  lockTake();
  out = s_data;
  lockGive();
}

static void weatherTask(void*);

static uint32_t cfgFingerprint(const Settings& s) {
  // Enough to notice any change that should trigger an immediate refetch.
  uint32_t fp = 2166136261u;
  auto mix = [&fp](uint32_t v) { fp = (fp ^ v) * 16777619u; };
  mix((uint32_t)(s.weather.lat * 10000.0f));
  mix((uint32_t)(s.weather.lon * 10000.0f));
  mix(s.weather.unitsF ? 1 : 2);
  mix(s.weather.pollSec);   // the task learns cadence via the same epoch bump
  return fp;
}

void weatherInit(const Settings& s) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  const uint32_t fp = cfgFingerprint(s);
  lockTake();
  s_cfg.lat     = s.weather.lat;
  s_cfg.lon     = s.weather.lon;
  s_cfg.unitsF  = s.weather.unitsF;
  s_cfg.pollSec = s.weather.pollSec;
  if (fp != s_cfgFp) {
    s_cfgFp = fp;
    s_cfg.epoch = s_cfg.epoch + 1;   // task refetches now
    s_data.error = false;            // a new place starts clean, not mid-complaint
  }
  lockGive();
  if (!s_task) {
    // Core 0 with the Spotify and calendar polls, away from the display loop.
    xTaskCreatePinnedToCore(weatherTask, "weather", 8192, nullptr, 1, &s_task, 0);
  }
}

static void buildQuery(float lat, float lon, bool unitsF, char* out, size_t n) {
  snprintf(out, n,
           WEATHER_PATH
           "?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min"
           "&timezone=auto&forecast_days=%d%s",
           (double)lat, (double)lon, WX_DAYS,
           unitsF ? "&temperature_unit=fahrenheit" : "");
}

// The device-path fetch, hand-rolled on purpose. The field cube sits behind a
// hotspot whose middlebox kills TLS handshakes to this host with a fatal
// alert (ssl=7780). What passes: a TLS connection opened to the resolved
// ADDRESS, so the SNI extension carries only an IP literal with nothing for
// a hostname blocklist to match - Open-Meteo then routes on the Host header
// (measured: 200 with the real JSON; it needs HTTP/1.1, so the chunked
// framing is undone here). Normal networks take this path too - the server
// accepts it either way. Plain http is the caller's fallback when even this
// TLS is killed: port 80 to this host was measured CLEAN on the field
// hotspot (the earlier "unexpected reply" was un-decoded chunking, fixed in
// NetFetch since).
static bool fetchDirect(float lat, float lon, bool unitsF, String& raw,
                        char* err, size_t errLen) {
  char path[280];
  buildQuery(lat, lon, unitsF, path, sizeof(path));

  IPAddress ip;
  if (!WiFi.hostByName(WEATHER_HOST, ip)) {
    snprintf(err, errLen, "DNS failed: %s", WEATHER_HOST);
    return false;
  }

  NetTlsGuard tlsLock;
  WiFiClientSecure sc;
  sc.setInsecure();
  sc.setTimeout(11);                       // seconds, like the netFetch path
  if (!sc.connect(ip.toString().c_str(), 443)) {
    char sb[4];
    const int ssl = sc.lastError(sb, sizeof(sb));
    snprintf(err, errLen, "tls @%s ssl=%X b=%uk", ip.toString().c_str(),
             (unsigned)(ssl < 0 ? -ssl : ssl),
             (unsigned)(platformMaxFreeBlock() / 1024));
    return false;
  }

  sc.print("GET ");
  sc.print(path);
  sc.print(" HTTP/1.1\r\nHost: " WEATHER_HOST
           "\r\nAccept: application/json\r\nConnection: close\r\n\r\n");

  String line = sc.readStringUntil('\n');
  const int sp = line.indexOf(' ');
  const int code = sp > 0 ? line.substring(sp + 1).toInt() : 0;
  if (code != 200) {
    snprintf(err, errLen, "HTTP %d", code);
    sc.stop();
    return false;
  }
  bool chunked = false;
  while (true) {
    line = sc.readStringUntil('\n');
    if (line.length() <= 1) break;         // blank line ends the headers
    line.toLowerCase();
    if (line.startsWith("transfer-encoding:") && line.indexOf("chunked") >= 0)
      chunked = true;
  }

  raw = "";
  raw.reserve(2048);
  const uint32_t deadline = millis() + 10000UL;
  if (chunked) {
    while ((int32_t)(millis() - deadline) < 0) {
      long sz = strtol(sc.readStringUntil('\n').c_str(), nullptr, 16);
      if (sz <= 0) break;                  // 0 chunk = done (or a read timeout)
      while (sz > 0 && (int32_t)(millis() - deadline) < 0 &&
             raw.length() < 8192) {
        char buf[257];
        const int want = sz < 256 ? (int)sz : 256;
        const int got = sc.read((uint8_t*)buf, want);
        if (got > 0) {
          buf[got] = 0;
          raw.concat(buf, got);
          sz -= got;
        } else if (!sc.connected() && !sc.available()) {
          break;
        } else {
          delay(1);
        }
      }
      sc.readStringUntil('\n');            // the chunk's trailing CRLF
    }
  } else {
    while ((sc.connected() || sc.available()) &&
           (int32_t)(millis() - deadline) < 0 && raw.length() < 8192) {
      while (sc.available() && raw.length() < 8192) raw += (char)sc.read();
      if (!sc.available()) delay(1);
    }
  }
  sc.stop();
  if (!raw.length()) {
    snprintf(err, errLen, "empty reply");
    return false;
  }
  return true;
}

static bool parseWeather(const String& raw, WeatherData& out) {
  // Filtered parse: the reply also carries units metadata and generation
  // stats nobody here reads.
  JsonDocument filter;
  filter["current"]["temperature_2m"] = true;
  filter["current"]["weather_code"] = true;
  filter["current"]["is_day"] = true;
  filter["daily"]["time"] = true;
  filter["daily"]["weather_code"] = true;
  filter["daily"]["temperature_2m_max"] = true;
  filter["daily"]["temperature_2m_min"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) !=
      DeserializationError::Ok)
    return false;
  if (!doc["current"].is<JsonObjectConst>() || !doc["daily"].is<JsonObjectConst>())
    return false;

  out.curTemp = doc["current"]["temperature_2m"] | 0.0f;
  out.curCode = (uint8_t)(doc["current"]["weather_code"] | 0);
  out.day     = (int)(doc["current"]["is_day"] | 1) != 0;

  JsonArrayConst tim = doc["daily"]["time"].as<JsonArrayConst>();
  JsonArrayConst cod = doc["daily"]["weather_code"].as<JsonArrayConst>();
  JsonArrayConst tmx = doc["daily"]["temperature_2m_max"].as<JsonArrayConst>();
  JsonArrayConst tmn = doc["daily"]["temperature_2m_min"].as<JsonArrayConst>();
  if (tmx.size() < 1 || tmn.size() < 1) return false;
  for (int i = 0; i < WX_DAYS; i++) {
    const bool have = (size_t)i < tmx.size();
    out.hi[i]   = have ? (tmx[i] | 0.0f) : 0.0f;
    out.lo[i]   = have ? (tmn[i] | 0.0f) : 0.0f;
    out.code[i] = have && (size_t)i < cod.size() ? (uint8_t)(cod[i] | 0) : 0;
    out.dow[i]  = have && (size_t)i < tim.size()
                      ? (int8_t)wxDowFromDate(tim[i] | "")
                      : (int8_t)-1;
  }
  return true;
}

static void weatherTask(void*) {
  uint32_t myEpoch = 0;
  uint32_t nextPollMs = 0;
  float    lat = 0.0f, lon = 0.0f;
  bool     unitsF = false;
  uint16_t pollSec = 600;

  for (;;) {
    if (myEpoch != s_cfg.epoch) {
      lockTake();
      myEpoch = s_cfg.epoch;
      lat     = s_cfg.lat;
      lon     = s_cfg.lon;
      unitsF  = s_cfg.unitsF;
      pollSec = s_cfg.pollSec;
      lockGive();
      nextPollMs = 0;                    // a new place: fetch now
    }

    // netHaveRoute, not the radio: tethered there is no station connection
    // and a perfectly good route. 0,0 is "no location set", not a real place.
    if ((lat == 0.0f && lon == 0.0f) || !netHaveRoute()) {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }
    const uint32_t now = millis();
    if (nextPollMs && (int32_t)(now - nextPollMs) < 0) {
      vTaskDelay(250 / portTICK_PERIOD_MS);
      continue;
    }

#if WITH_SPOTIFY
    // One TLS operation at a time across the firmware — the same rule the
    // calendar task and the art fetch already obey, for the same heap.
    if (!spotifyNetLock(10000)) { vTaskDelay(500 / portTICK_PERIOD_MS); continue; }
#endif
    String raw;
    bool fetched;
    char ferr[72] = "";
    if (netFetchTethered()) {
      // The browser end speaks TLS 1.3 through whatever the network filters,
      // so the tether path stays an ordinary https fetch.
      char url[300];
      snprintf(url, sizeof(url), "https://" WEATHER_HOST "%s", "");
      char q[280];
      buildQuery(lat, lon, unitsF, q, sizeof(q));
      strlcat(url, q, sizeof(url));
      const NetFetchResult r = netFetchToString(url, false, "Accept: application/json",
                                                nullptr, 0, raw, 8192, 10000);
      fetched = r.ok;
      if (!fetched) strlcpy(ferr, r.error, sizeof(ferr));
    } else {
      fetched = fetchDirect(lat, lon, unitsF, raw, ferr, sizeof(ferr));
      if (!fetched) {
        // TLS is being interfered with even without a hostname to match: fall
        // back to plain http, which netFetch now de-chunks correctly. Public
        // weather data over port 80 beats a blank screen.
        char url[300];
        snprintf(url, sizeof(url), "http://" WEATHER_HOST);
        char q[280];
        buildQuery(lat, lon, unitsF, q, sizeof(q));
        strlcat(url, q, sizeof(url));
        const NetFetchResult r = netFetchToString(url, false, "Accept: application/json",
                                                  nullptr, 0, raw, 8192, 10000);
        if (r.ok) {
          fetched = true;
        } else {
          char both[72];
          snprintf(both, sizeof(both), "%.32s / http: %.28s", ferr, r.error);
          strlcpy(ferr, both, sizeof(ferr));
        }
      }
    }
#if WITH_SPOTIFY
    spotifyNetUnlock();
#endif

    // Parse into a staging copy and swap it in whole, so the display loop can
    // never read a half-written forecast.
    WeatherData fresh = {};
    if (fetched && parseWeather(raw, fresh)) {
      fresh.valid = true;
      fresh.error = false;
      fresh.lastOkMs = millis();
      lockTake();
      s_data = fresh;
      lockGive();
      nextPollMs = millis() + (uint32_t)pollSec * 1000UL;
    } else {
      lockTake();
      s_data.error = true;   // the last good reading stays; the red dot says stale
      if (fetched)
        // Show the head of what DID come back - a filter's block page names
        // itself in its first line.
        snprintf(s_data.errMsg, sizeof(s_data.errMsg), "reply: %.60s", raw.c_str());
      else
        strlcpy(s_data.errMsg, ferr, sizeof(s_data.errMsg));
      lockGive();
      nextPollMs = millis() + 60000UL;   // errors retry in a minute, not a poll period
    }
  }
}

#endif  // WITH_WEATHER
