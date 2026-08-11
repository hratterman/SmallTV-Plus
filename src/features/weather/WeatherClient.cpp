#include "WeatherClient.h"
#if WITH_WEATHER

#include <ArduinoJson.h>
#include "NetFetch.h"

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

static void buildUrl(const Settings& s, char* out, size_t n) {
  snprintf(out, n,
           WEATHER_URL
           "?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min"
           "&timezone=auto&forecast_days=%d%s",
           (double)s.weather.lat, (double)s.weather.lon, WX_DAYS,
           s.weather.unitsF ? "&temperature_unit=fahrenheit" : "");
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

  char url[280];
  buildUrl(s, url, sizeof(url));
  String raw;
  const NetFetchResult r = netFetchToString(url, false, "Accept: application/json",
                                            nullptr, 0, raw, 8192, 10000);
  if (r.ok && parseWeather(raw)) {
    s_data.valid = true;
    s_data.error = false;
    s_data.lastOkMs = now;
    s_nextPollMs = now + (uint32_t)s.weather.pollSec * 1000UL;
  } else {
    s_data.error = true;
    strlcpy(s_data.errMsg, r.ok ? "unexpected reply" : r.error, sizeof(s_data.errMsg));
    s_nextPollMs = now + 60000UL;     // errors retry in a minute, not a poll period
  }
}

#endif  // WITH_WEATHER
