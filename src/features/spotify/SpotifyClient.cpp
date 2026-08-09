#include "SpotifyClient.h"
#include "AlbumArt.h"   // SPOTIFY_ART_PX: the size the screen wants
#if WITH_SPOTIFY

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// The poll runs on its own task, pinned to core 0, for one reason: a TLS
// handshake to Spotify takes a few hundred milliseconds and it used to happen
// inside the render loop. Every ten seconds the screen simply stopped — barely
// noticeable on a 1 Hz ticker, obvious on anything animating, and fatal to a
// game. Nothing about the poll needs to be on the drawing thread; it only
// needed to stop pretending it did.
static SpotifyData s_data;              // written by the poll task, under s_lock
static SpotifyData s_view;              // snapshot handed to the render task
static SemaphoreHandle_t s_lock = nullptr;
static TaskHandle_t s_task = nullptr;

static String   s_accessToken;
static uint32_t s_tokenExpiresAt = 0;   // millis() when the access token dies
static volatile bool s_forced = false;
// Read by the poll task; a plain word written once per config change.
static volatile uint16_t s_httpTimeout = DEFAULT_HTTP_TIMEOUT;

// What the task works from, so it never reads Settings while the web server is
// writing them. Same shape as the miner's config snapshot.
static struct {
  volatile uint32_t epoch;
  bool     enabled;
  uint16_t pollSec;
  String   clientId, clientSecret, refreshToken;
} s_cfg;

static inline void lockTake() { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void lockGive() { if (s_lock) xSemaphoreGive(s_lock); }

void spotifyForceRefresh() { s_forced = true; }

// Hands back a consistent copy rather than the live struct: the poll task can
// be halfway through writing a track name at any moment.
const SpotifyData& spotifyGet() {
  if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
    s_view = s_data;
    xSemaphoreGive(s_lock);
  }
  return s_view;
}

static void spotifyTask(void*);

void spotifyInit(const Settings& s) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  lockTake();
  memset(&s_data, 0, sizeof(s_data));
  s_cfg.enabled      = s.spotify.enabled;
  s_cfg.pollSec      = s.spotify.pollSec;
  s_cfg.clientId     = s.spotify.clientId;
  s_cfg.clientSecret = s.spotify.clientSecret;
  s_cfg.refreshToken = s.spotify.refreshToken;
  s_cfg.epoch++;
  s_httpTimeout = s.httpTimeout;
  lockGive();
  s_accessToken = "";
  s_tokenExpiresAt = 0;
  if (!s_task) {
    // Core 0, away from the Arduino loop on core 1, so a handshake never costs
    // the display a frame. It shares core 0 with the hardware hash worker and
    // takes a slice of it for the half-second a poll lasts.
    xTaskCreatePinnedToCore(spotifyTask, "spotify", 10240, nullptr, 1, &s_task, 0);
  }
}

static void setError(const char* msg) {
  lockTake();
  s_data.error = true;
  strlcpy(s_data.errorMsg, msg, sizeof(s_data.errorMsg));
  lockGive();
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
static bool refreshAccessToken(const String& clientId, const String& clientSecret,
                               const String& refreshToken) {
  WiFiClientSecure client;
  client.setInsecure();          // same posture as the GitHub self-updater
  client.setTimeout(s_httpTimeout / 1000 + 1);

  HTTPClient http;
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) {
    setError("token: connect failed");
    return false;
  }
  http.setTimeout(s_httpTimeout);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Authorization",
                 "Basic " + base64(clientId + ":" + clientSecret));

  String body = "grant_type=refresh_token&refresh_token=" + refreshToken;
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

static bool fetchNowPlaying() {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(s_httpTimeout / 1000 + 1);

  HTTPClient http;
  if (!http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing")) {
    setError("player: connect failed");
    return false;
  }
  http.setTimeout(s_httpTimeout);
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
  // The art list is three entries of {url,width,height}; small enough to keep
  // now that the rest of the payload is filtered away.
  JsonObject img = item["album"]["images"][0].to<JsonObject>();
  img["url"] = true;
  img["width"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { setError("player: bad JSON"); return false; }

  lockTake();
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

  // Pick the cover closest to the size the screen wants once TJpgDec's power-of
  // two descaling is applied. Spotify offers 640/300/64, so 300 at 1/2 and 640
  // at 1/4 both land near the target; preferring the smaller source keeps the
  // download short, and the download is the slow part.
  s_data.artUrl[0] = 0;
  s_data.artPx = 0;
  {
    int bestErr = 1 << 30;
    uint32_t bestBytes = 0;
    for (JsonObjectConst im : doc["item"]["album"]["images"].as<JsonArrayConst>()) {
      const char* u = im["url"] | "";
      const int w = im["width"] | 0;
      if (!u[0] || w <= 0 || strlen(u) >= SPOTIFY_ART_LEN) continue;
      for (int sc = 0; sc <= 3; sc++) {
        const int got = w >> sc;
        const int e = got > SPOTIFY_ART_PX ? (got - SPOTIFY_ART_PX) * 2   // upscaling is worse
                                           : (SPOTIFY_ART_PX - got);
        if (e < bestErr || (e == bestErr && (uint32_t)w < bestBytes)) {
          bestErr = e;
          bestBytes = (uint32_t)w;
          strlcpy(s_data.artUrl, u, sizeof(s_data.artUrl));
          s_data.artPx = (uint16_t)w;
        }
      }
    }
  }

  s_data.valid = true;
  s_data.error = false;
  s_data.lastOkMs = millis();
  s_data.startedAtMs = millis();
  lockGive();
  return true;
}

static void spotifyTask(void*) {
  uint32_t myEpoch = 0, lastPoll = 0;
  bool     enabled = false;
  uint16_t pollSec = DEFAULT_SPOTIFY_POLL_SEC;
  String   cid, csec, rtok;

  for (;;) {
    if (myEpoch != s_cfg.epoch) {
      lockTake();
      myEpoch = s_cfg.epoch;
      enabled = s_cfg.enabled;
      pollSec = s_cfg.pollSec;
      cid  = s_cfg.clientId;
      csec = s_cfg.clientSecret;
      rtok = s_cfg.refreshToken;
      lockGive();
      s_accessToken = "";
      lastPoll = 0;
    }

    if (!enabled || !rtok.length() || WiFi.status() != WL_CONNECTED) {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }

    const uint32_t now = millis();
    if (!s_forced && lastPoll && (now - lastPoll) < (uint32_t)pollSec * 1000UL) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }
    s_forced = false;
    lastPoll = now;

    if (!s_accessToken.length() || (int32_t)(now - s_tokenExpiresAt) >= 0) {
      if (!refreshAccessToken(cid, csec, rtok)) {
        vTaskDelay(3000 / portTICK_PERIOD_MS);   // back off; do not hammer
        continue;
      }
    }
    fetchNowPlaying();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// Kept so the loop's call site stays honest about there being nothing to do:
// polling belongs to the task above and has since the handshake started costing
// the display frames.
void spotifyService(const Settings& s) { (void)s; }

uint32_t spotifyProgressNow() {
  const SpotifyData& d = spotifyGet();
  if (!d.valid || !d.playing) return d.progressMs;
  uint32_t p = d.progressMs + (millis() - d.startedAtMs);
  return (d.durationMs && p > d.durationMs) ? d.durationMs : p;
}

bool spotifyIsPlaying(const Settings& s) {
  const SpotifyData& d = spotifyGet();
  if (!s.spotify.enabled || !d.valid || !d.playing) return false;
  // Don't hold focus on a stale reading: if polling has stopped working, let the
  // other modes have the screen back rather than freezing on an old track.
  const uint32_t stale = (uint32_t)s.spotify.pollSec * 1000UL * 3UL + 15000UL;
  return (millis() - d.lastOkMs) < stale;
}

#endif  // WITH_SPOTIFY
