#include "SpotifyClient.h"
#if WITH_SPOTIFY

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

static SpotifyData s_data;
static String   s_accessToken;
static uint32_t s_tokenExpiresAt = 0;   // millis() when the access token dies
static uint32_t s_lastPoll = 0;
static bool     s_forced = false;

void spotifyForceRefresh() { s_forced = true; }
const SpotifyData& spotifyGet() { return s_data; }

void spotifyInit(const Settings& s) {
  memset(&s_data, 0, sizeof(s_data));
  s_accessToken = "";
  s_tokenExpiresAt = 0;
  s_lastPoll = 0;
}

static void setError(const char* msg) {
  s_data.error = true;
  strlcpy(s_data.errorMsg, msg, sizeof(s_data.errorMsg));
}

// Basic-auth header for the token endpoint. Small enough to do by hand rather
// than pull in a base64 library that differs between the Arduino cores.
static String base64(const String& in) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  out.reserve(((in.length() + 2) / 3) * 4 + 1);
  size_t i = 0;
  while (i + 2 < in.length()) {
    uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
    out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
    out += T[(v >> 6) & 63];  out += T[v & 63];
    i += 3;
  }
  if (i < in.length()) {
    uint32_t v = (uint8_t)in[i] << 16;
    bool two = (i + 1 < in.length());
    if (two) v |= (uint8_t)in[i + 1] << 8;
    out += T[(v >> 18) & 63];
    out += T[(v >> 12) & 63];
    out += two ? T[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

// Trade the refresh token for an access token. Spotify's refresh tokens do not
// expire on their own, so this is the only long-lived secret the device holds.
static bool refreshAccessToken(const Settings& s) {
  WiFiClientSecure client;
  client.setInsecure();          // same posture as the GitHub self-updater
  client.setTimeout(s.httpTimeout / 1000 + 1);

  HTTPClient http;
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) {
    setError("token: connect failed");
    return false;
  }
  http.setTimeout(s.httpTimeout);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization",
                 "Basic " + base64(s.spotify.clientId + ":" + s.spotify.clientSecret));

  String body = "grant_type=refresh_token&refresh_token=" + s.spotify.refreshToken;
  int code = http.POST(body);
  if (code != 200) {
    char m[48];
    snprintf(m, sizeof(m), "token: HTTP %d", code);
    setError(m);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { setError("token: bad JSON"); return false; }

  const char* tok = doc["access_token"] | "";
  if (!tok[0]) { setError("token: none returned"); return false; }
  s_accessToken = tok;
  // Renew a minute early so a poll never races the expiry.
  const uint32_t ttl = doc["expires_in"] | 3600;
  s_tokenExpiresAt = millis() + (ttl > 90 ? (ttl - 60) : 30) * 1000UL;
  return true;
}

static bool fetchNowPlaying(const Settings& s) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(s.httpTimeout / 1000 + 1);

  HTTPClient http;
  if (!http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing")) {
    setError("player: connect failed");
    return false;
  }
  http.setTimeout(s.httpTimeout);
  http.addHeader("Authorization", "Bearer " + s_accessToken);

  int code = http.GET();

  // 204 is Spotify's "nothing is playing" — a normal state, not a failure.
  if (code == 204) {
    http.end();
    s_data.valid = true;
    s_data.error = false;
    s_data.playing = false;
    s_data.track[0] = s_data.artist[0] = 0;
    s_data.lastOkMs = millis();
    return true;
  }
  if (code == 401) {          // access token died early; force a refresh next tick
    http.end();
    s_tokenExpiresAt = 0;
    setError("player: token rejected");
    return false;
  }
  if (code != 200) {
    char m[48];
    snprintf(m, sizeof(m), "player: HTTP %d", code);
    setError(m);
    http.end();
    return false;
  }

  // The full payload is several KB of album art URLs and market lists. A filter
  // keeps only what the screen shows, so the parse stays small on the heap the
  // miner and the web server are also using.
  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  JsonObject item = filter["item"].to<JsonObject>();
  item["name"] = true;
  item["duration_ms"] = true;
  item["artists"][0]["name"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { setError("player: bad JSON"); return false; }

  s_data.playing    = doc["is_playing"] | false;
  s_data.progressMs = doc["progress_ms"] | 0;
  s_data.durationMs = doc["item"]["duration_ms"] | 0;
  strlcpy(s_data.track, doc["item"]["name"] | "", sizeof(s_data.track));

  // Join however many artists there are, comma separated, until the field fills.
  s_data.artist[0] = 0;
  for (JsonObjectConst a : doc["item"]["artists"].as<JsonArrayConst>()) {
    const char* n = a["name"] | "";
    if (!n[0]) continue;
    if (s_data.artist[0]) strlcat(s_data.artist, ", ", sizeof(s_data.artist));
    strlcat(s_data.artist, n, sizeof(s_data.artist));
  }

  s_data.valid = true;
  s_data.error = false;
  s_data.lastOkMs = millis();
  s_data.startedAtMs = millis();
  return true;
}

void spotifyService(const Settings& s) {
  if (!s.spotify.enabled || !s.spotify.refreshToken.length()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  const uint32_t now = millis();
  const uint32_t period = (uint32_t)s.spotify.pollSec * 1000UL;
  if (!s_forced && s_lastPoll && (now - s_lastPoll) < period) return;
  s_forced = false;
  s_lastPoll = now;

  if (!s_accessToken.length() || (int32_t)(now - s_tokenExpiresAt) >= 0) {
    if (!refreshAccessToken(s)) return;
  }
  fetchNowPlaying(s);
}

uint32_t spotifyProgressNow() {
  if (!s_data.valid || !s_data.playing) return s_data.progressMs;
  uint32_t p = s_data.progressMs + (millis() - s_data.startedAtMs);
  return (s_data.durationMs && p > s_data.durationMs) ? s_data.durationMs : p;
}

bool spotifyIsPlaying(const Settings& s) {
  if (!s.spotify.enabled || !s_data.valid || !s_data.playing) return false;
  // Don't hold focus on a stale reading: if polling has stopped working, let the
  // other modes have the screen back rather than freezing on an old track.
  const uint32_t stale = (uint32_t)s.spotify.pollSec * 1000UL * 3UL + 15000UL;
  return (millis() - s_data.lastOkMs) < stale;
}

#endif  // WITH_SPOTIFY
