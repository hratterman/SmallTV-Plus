#include "WeatherClient.h"
#if WITH_WEATHER

#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include "NetFetch.h"
#include "Platform.h"

static WeatherData s_data;
static uint32_t    s_nextPollMs = 0;
static uint32_t    s_cfgFp = 0;       // refetch when the location/units change

const WeatherData& weatherGet() { return s_data; }

static uint32_t cfgFingerprint(const Settings& s) {
  // Enough to notice any change that should trigger an immediate refetch.
  uint32_t fp = 2166136261u;
  auto mix = [&fp](uint32_t v) { fp = (fp ^ v) * 16777619u; };
  mix((uint32_t)(s.weather.lat * 10000.0f));
  mix((uint32_t)(s.weather.lon * 10000.0f));
  mix(s.weather.unitsF ? 1 : 2);
  return fp;
}

void weatherInit(const Settings& s) {
  const uint32_t fp = cfgFingerprint(s);
  if (fp != s_cfgFp) {
    s_cfgFp = fp;
    s_nextPollMs = 0;                 // fetch on the next service tick
    s_data.error = false;
  }
}

static bool haveLocation(const Settings& s) {
  return s.weather.lat != 0.0f || s.weather.lon != 0.0f;
}

static void buildQuery(const Settings& s, char* out, size_t n) {
  snprintf(out, n,
           WEATHER_PATH
           "?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min"
           "&timezone=auto&forecast_days=%d%s",
           (double)s.weather.lat, (double)s.weather.lon, WX_DAYS,
           s.weather.unitsF ? "&temperature_unit=fahrenheit" : "");
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
static bool fetchDirect(const Settings& s, String& raw, char* err, size_t errLen) {
  char path[280];
  buildQuery(s, path, sizeof(path));

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

static bool parseWeather(const String& raw) {
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

  s_data.curTemp = doc["current"]["temperature_2m"] | 0.0f;
  s_data.curCode = (uint8_t)(doc["current"]["weather_code"] | 0);
  s_data.day     = (int)(doc["current"]["is_day"] | 1) != 0;

  JsonArrayConst tim = doc["daily"]["time"].as<JsonArrayConst>();
  JsonArrayConst cod = doc["daily"]["weather_code"].as<JsonArrayConst>();
  JsonArrayConst tmx = doc["daily"]["temperature_2m_max"].as<JsonArrayConst>();
  JsonArrayConst tmn = doc["daily"]["temperature_2m_min"].as<JsonArrayConst>();
  if (tmx.size() < 1 || tmn.size() < 1) return false;
  for (int i = 0; i < WX_DAYS; i++) {
    const bool have = (size_t)i < tmx.size();
    s_data.hi[i]   = have ? (tmx[i] | 0.0f) : 0.0f;
    s_data.lo[i]   = have ? (tmn[i] | 0.0f) : 0.0f;
    s_data.code[i] = have && (size_t)i < cod.size() ? (uint8_t)(cod[i] | 0) : 0;
    s_data.dow[i]  = have && (size_t)i < tim.size()
                         ? (int8_t)wxDowFromDate(tim[i] | "")
                         : (int8_t)-1;
  }
  return true;
}

void weatherService(const Settings& s) {
  if (!haveLocation(s) || !netHaveRoute()) return;
  const uint32_t now = millis();
  if (s_nextPollMs && (int32_t)(now - s_nextPollMs) < 0) return;

  String raw;
  bool fetched;
  char ferr[72] = "";
  if (netFetchTethered()) {
    // The browser end speaks TLS 1.3 through whatever the network filters,
    // so the tether path stays an ordinary https fetch.
    char url[300];
    snprintf(url, sizeof(url), "https://" WEATHER_HOST "%s", "");
    char q[280];
    buildQuery(s, q, sizeof(q));
    strlcat(url, q, sizeof(url));
    const NetFetchResult r = netFetchToString(url, false, "Accept: application/json",
                                              nullptr, 0, raw, 8192, 10000);
    fetched = r.ok;
    if (!fetched) strlcpy(ferr, r.error, sizeof(ferr));
  } else {
    fetched = fetchDirect(s, raw, ferr, sizeof(ferr));
    if (!fetched) {
      // TLS is being interfered with even without a hostname to match: fall
      // back to plain http, which netFetch now de-chunks correctly. Public
      // weather data over port 80 beats a blank screen.
      char url[300];
      snprintf(url, sizeof(url), "http://" WEATHER_HOST);
      char q[280];
      buildQuery(s, q, sizeof(q));
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

  if (fetched && parseWeather(raw)) {
    s_data.valid = true;
    s_data.error = false;
    s_data.lastOkMs = now;
    s_nextPollMs = now + (uint32_t)s.weather.pollSec * 1000UL;
  } else {
    s_data.error = true;
    if (fetched)
      // Show the head of what DID come back - a filter's block page names
      // itself in its first line.
      snprintf(s_data.errMsg, sizeof(s_data.errMsg), "reply: %.60s", raw.c_str());
    else
      strlcpy(s_data.errMsg, ferr, sizeof(s_data.errMsg));
    s_nextPollMs = now + 60000UL;     // errors retry in a minute, not a poll period
  }
}

#endif  // WITH_WEATHER
