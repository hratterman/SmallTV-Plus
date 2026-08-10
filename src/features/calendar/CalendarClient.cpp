#include "config.h"
#if WITH_CALENDAR

#include "CalendarClient.h"
#include "CalendarTime.h"
#include "NetFetch.h"
#include "Clock.h"
#include <ArduinoJson.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#if WITH_SPOTIFY
#include "SpotifyClient.h"   // spotifyNetLock: one TLS operation at a time
#endif

// ---------------------------------------------------------------------------
// Shared state, guarded by s_lock. The task writes, the main loop reads.
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_lock = nullptr;
static TaskHandle_t      s_task = nullptr;

static CalSnapshot s_snap = {};
static uint32_t    s_okAtMs = 0;
static String      s_rotatedToken;

static struct {
  volatile uint32_t epoch;
  bool     enabled;
  uint8_t  provider;
  String   clientId, clientSecret, refreshToken;
  uint16_t pollSec;
  uint16_t httpTimeout;
} s_cfg;

static inline void lockTake() { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void lockGive() { if (s_lock) xSemaphoreGive(s_lock); }

static void setError(const char* m) {
  lockTake();
  strlcpy(s_snap.error, m, sizeof(s_snap.error));
  lockGive();
  if (m[0]) Serial.printf("[calendar] %s\n", m);
}

// ---------------------------------------------------------------------------
// Small pieces (task-side only)
// ---------------------------------------------------------------------------
static void urlEnc(const char* in, String& out) {
  static const char* hex = "0123456789ABCDEF";
  for (const char* p = in; *p; p++) {
    const char c = *p;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[((uint8_t)c) >> 4];
      out += hex[((uint8_t)c) & 0xF];
    }
  }
}

// "YYYY-MM-DDTHH:MM:SSZ" from epoch seconds, colons already %-encoded for a
// query string.
static void isoUtc(int64_t t, String& out) {
  struct tm g;
  time_t tt = (time_t)t;
  gmtime_r(&tt, &g);
  // The %u casts bound every field so -Wformat-truncation can prove the
  // worst case fits; tm's ints are in range but the compiler cannot know it.
  char b[40];
  snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u%%3A%02u%%3A%02uZ",
           (unsigned)(g.tm_year + 1900) % 10000u, (unsigned)(g.tm_mon + 1) % 100u,
           (unsigned)g.tm_mday % 100u, (unsigned)g.tm_hour % 100u,
           (unsigned)g.tm_min % 100u, (unsigned)g.tm_sec % 100u);
  out += b;
}

// The cube's own offset from UTC right now, for planting all-day events at
// local midnight. Difference of localtime and gmtime of one instant, so DST is
// already folded in.
static int localOffsetMin() {
  const time_t now = time(nullptr);
  struct tm lt, gt;
  localtime_r(&now, &lt);
  gmtime_r(&now, &gt);
  const int64_t l = calDaysFromCivil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 1440LL +
                    lt.tm_hour * 60 + lt.tm_min;
  const int64_t g = calDaysFromCivil(gt.tm_year + 1900, gt.tm_mon + 1, gt.tm_mday) * 1440LL +
                    gt.tm_hour * 60 + gt.tm_min;
  return (int)(l - g);
}

// ---------------------------------------------------------------------------
// Token refresh (task-side; cfg fields passed in, already snapshotted)
// ---------------------------------------------------------------------------
static String   s_accessToken;
static uint32_t s_tokenDiesMs = 0;

static bool refreshAccessToken(uint8_t provider, const String& cid,
                               const String& csec, String& rtok,
                               uint16_t timeoutMs) {
  String body = "grant_type=refresh_token&client_id=";
  urlEnc(cid.c_str(), body);
  body += "&refresh_token=";
  urlEnc(rtok.c_str(), body);
  const char* url;
  if (provider == CAL_MICROSOFT) {
    url = MS_TOKEN_URL;
    body += "&scope=Calendars.Read%20offline_access";
  } else {
    url = GOOGLE_TOKEN_URL;
    body += "&client_secret=";
    urlEnc(csec.c_str(), body);
  }

  String raw;
  const NetFetchResult res = netFetchToString(
      url, /*post=*/true, "Content-Type: application/x-www-form-urlencoded",
      (const uint8_t*)body.c_str(), (uint16_t)body.length(),
      raw, 6144, timeoutMs);
  if (!res.ok) {
    char m[64];
    snprintf(m, sizeof(m), "token: %.48s", res.error);
    setError(m);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, raw)) { setError("token: bad JSON"); return false; }
  const char* tok = doc["access_token"] | "";
  if (!tok[0]) {
    // The provider says why in error_description — the one string that tells
    // "expired, re-run the helper" apart from "wrong client id".
    char m[64];
    snprintf(m, sizeof(m), "token: %.48s",
             (const char*)(doc["error_description"] | doc["error"] | "refused"));
    setError(m);
    return false;
  }
  s_accessToken = tok;
  const long ttl = doc["expires_in"] | 3600;
  s_tokenDiesMs = millis() + (uint32_t)((ttl > 90 ? ttl - 60 : 30) * 1000L);

  // Microsoft rotates; keep the replacement for main.cpp to persist so a
  // reboot months from now still holds a live token.
  const char* rot = doc["refresh_token"] | "";
  if (rot[0] && rtok != rot) {
    lockTake();
    s_rotatedToken = rot;
    lockGive();
    rtok = rot;                            // use it ourselves from now on too
  }
  return true;
}

// ---------------------------------------------------------------------------
// Events fetch (task-side, into a staging list; swapped in under the lock)
// ---------------------------------------------------------------------------
static CalEvent s_stage[CAL_MAX_EVENTS];
static uint8_t  s_stageN = 0;

static void stageEvent(const char* title, const char* startStr, const char* endStr,
                       int offMin, bool forceAllDay) {
  if (s_stageN >= CAL_MAX_EVENTS || !startStr || !startStr[0]) return;
  CalEvent& e = s_stage[s_stageN];
  bool allDay = false;
  if (!calIsoToUtc(startStr, offMin, &e.startUtc, &allDay)) return;
  // A missing end is legal on odd Graph items; zero-length then.
  if (!endStr || !endStr[0] || !calIsoToUtc(endStr, offMin, &e.endUtc, nullptr))
    e.endUtc = e.startUtc;
  e.allDay = allDay || forceAllDay;
  strlcpy(e.title, (title && title[0]) ? title : "(untitled)", sizeof(e.title));
  s_stageN++;
}

static void sortStage() {
  // Both providers say they sort; neither is trusted, because an unsorted list
  // makes "next" wrong in a way that looks like a missing meeting. n <= 6.
  for (uint8_t i = 1; i < s_stageN; i++)
    for (uint8_t j = i; j && s_stage[j].startUtc < s_stage[j - 1].startUtc; j--) {
      CalEvent t = s_stage[j];
      s_stage[j] = s_stage[j - 1];
      s_stage[j - 1] = t;
    }
}

static bool fetchEvents(uint8_t provider, uint16_t timeoutMs) {
  const int64_t now = (int64_t)time(nullptr);
  String url;
  String hdrs = "Authorization: Bearer " + s_accessToken;

  if (provider == CAL_MICROSOFT) {
    url = MS_CAL_URL "?startDateTime=";
    isoUtc(now, url);
    url += "&endDateTime=";
    isoUtc(now + (int64_t)CAL_LOOKAHEAD_SEC, url);
    url += "&$select=subject,start,end,isAllDay&$orderby=start/dateTime&$top=";
    url += CAL_MAX_EVENTS;
    // Graph otherwise answers in the mailbox's zone with no offset in the
    // string; pinned to UTC, the naked timestamps mean one thing.
    hdrs += "\nPrefer: outlook.timezone=\"UTC\"";
  } else {
    url = GOOGLE_CAL_URL "?singleEvents=true&orderBy=startTime&maxResults=";
    url += CAL_MAX_EVENTS;
    url += "&fields=items(summary,start,end)&timeMin=";
    isoUtc(now, url);
    url += "&timeMax=";
    isoUtc(now + (int64_t)CAL_LOOKAHEAD_SEC, url);
  }

  String raw;
  const NetFetchResult res = netFetchToString(url.c_str(), false, hdrs.c_str(),
                                              nullptr, 0, raw, 10240, timeoutMs);
  if (!res.ok) {
    char m[64];
    snprintf(m, sizeof(m), "events: %.48s", res.error);
    setError(m);
    return false;
  }
  if (res.status == 401) {          // token died early; refresh on the next tick
    s_tokenDiesMs = 0;
    s_accessToken = "";
    setError("events: token rejected");
    return false;
  }
  if (res.status != 200) {
    char m[32];
    snprintf(m, sizeof(m), "events: HTTP %d", res.status);
    setError(m);
    return false;
  }

  JsonDocument filter;
  if (provider == CAL_MICROSOFT) {
    JsonObject it = filter["value"][0].to<JsonObject>();
    it["subject"] = true;
    it["isAllDay"] = true;
    it["start"]["dateTime"] = true;
    it["end"]["dateTime"] = true;
  } else {
    JsonObject it = filter["items"][0].to<JsonObject>();
    it["summary"] = true;
    it["start"]["dateTime"] = true;
    it["start"]["date"] = true;
    it["end"]["dateTime"] = true;
    it["end"]["date"] = true;
  }

  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter))) {
    setError("events: bad JSON");
    return false;
  }

  const int offMin = localOffsetMin();   // all-day dates; timed ones carry zones
  s_stageN = 0;
  if (provider == CAL_MICROSOFT) {
    for (JsonObjectConst it : doc["value"].as<JsonArrayConst>())
      stageEvent(it["subject"] | "", it["start"]["dateTime"] | "",
                 it["end"]["dateTime"] | "", 0, it["isAllDay"] | false);
  } else {
    for (JsonObjectConst it : doc["items"].as<JsonArrayConst>()) {
      const char* st = it["start"]["dateTime"] | (const char*)(it["start"]["date"] | "");
      const char* en = it["end"]["dateTime"] | (const char*)(it["end"]["date"] | "");
      stageEvent(it["summary"] | "", st, en, offMin, false);
    }
  }
  sortStage();

  lockTake();
  memcpy(s_snap.events, s_stage, sizeof(s_snap.events));
  s_snap.count = s_stageN;
  s_snap.ok = true;
  s_snap.error[0] = 0;
  s_okAtMs = millis();
  lockGive();
  return true;
}

// ---------------------------------------------------------------------------
// The task
// ---------------------------------------------------------------------------
// The link flow lives with the other public surface below; the task only needs
// these three names.
static volatile bool s_linkReq = false;
static void linkSet(CalLinkPhase ph, const char* code, const char* url, const char* msg);
static void runLinkFlow(const String& cid, uint16_t timeoutMs);

static void calendarTask(void*) {
  uint32_t myEpoch = 0;
  uint32_t lastPoll = 0;
  bool     enabled = false;
  uint8_t  provider = CAL_GOOGLE;
  uint16_t pollSec = DEFAULT_CALENDAR_POLL_SEC;
  uint16_t timeoutMs = 10000;
  String   cid, csec, rtok;

  for (;;) {
    if (myEpoch != s_cfg.epoch) {
      lockTake();
      myEpoch  = s_cfg.epoch;
      enabled  = s_cfg.enabled;
      provider = s_cfg.provider;
      pollSec  = s_cfg.pollSec;
      cid      = s_cfg.clientId;
      csec     = s_cfg.clientSecret;
      rtok     = s_cfg.refreshToken;
      timeoutMs = s_cfg.httpTimeout;
      s_snap.ok = false;                 // a new account's events, not the old one's
      s_snap.count = 0;
      lockGive();
      s_accessToken = "";
      s_tokenDiesMs = 0;
      lastPoll = 0;
    }

    // A link request runs regardless of enabled/token state — linking is how
    // those come to exist in the first place. It only needs a route and an id.
    if (s_linkReq) {
      s_linkReq = false;
      if (!cid.length())       linkSet(CAL_LINK_FAILED, 0, 0, "set the client ID first");
      else if (!netHaveRoute()) linkSet(CAL_LINK_FAILED, 0, 0, "no network route");
      else                      runLinkFlow(cid, timeoutMs);
      continue;
    }

    // netHaveRoute, not the radio: tethered there is no station connection and
    // a perfectly good route. "Next" also needs a clock that has been set.
    if (!enabled || !rtok.length() || !cid.length() ||
        !netHaveRoute() || !clockSynced()) {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }

    const uint32_t now = millis();
    if (lastPoll && (now - lastPoll) < (uint32_t)pollSec * 1000UL) {
      vTaskDelay(250 / portTICK_PERIOD_MS);
      continue;
    }
    lastPoll = now;

#if WITH_SPOTIFY
    // One TLS operation at a time across the firmware — the same rule the art
    // fetch already obeys, for the same heap.
    if (!spotifyNetLock(10000)) { vTaskDelay(500 / portTICK_PERIOD_MS); continue; }
#endif
    bool ok = true;
    if (!s_accessToken.length() || (int32_t)(now - s_tokenDiesMs) >= 0)
      ok = refreshAccessToken(provider, cid, csec, rtok, timeoutMs);
    if (ok) ok = fetchEvents(provider, timeoutMs);
#if WITH_SPOTIFY
    spotifyNetUnlock();
#endif

    if (!ok) {
      // Retry sooner than the poll period, but never hammer a refusing
      // provider: 30 s floor.
      lastPoll = now - (uint32_t)pollSec * 1000UL + 30000UL;
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }
}

// ---------------------------------------------------------------------------
// Public surface (main-loop side)
// ---------------------------------------------------------------------------
void calendarInit(const Settings& s) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  lockTake();
  s_cfg.enabled      = s.calendar.enabled;
  s_cfg.provider     = s.calendar.provider;
  s_cfg.clientId     = s.calendar.clientId;
  s_cfg.clientSecret = s.calendar.clientSecret;
  s_cfg.refreshToken = s.calendar.refreshToken;
  s_cfg.pollSec      = s.calendar.pollSec;
  s_cfg.httpTimeout  = s.httpTimeout;
  s_cfg.epoch = s_cfg.epoch + 1;
  lockGive();
  if (!s_task) {
    // Core 0 with the Spotify poll, away from the display on core 1.
    xTaskCreatePinnedToCore(calendarTask, "calendar", 10240, nullptr, 1, &s_task, 0);
  }
}

void calendarSnapshot(CalSnapshot& out) {
  lockTake();
  out = s_snap;
  out.ageMs = s_snap.ok ? millis() - s_okAtMs : 0;
  lockGive();
}

String calendarTakeRotatedToken() {
  lockTake();
  String t = s_rotatedToken;
  s_rotatedToken = "";
  lockGive();
  return t;
}

// ---------------------------------------------------------------------------
// On-device linking (Microsoft device-code flow)
// ---------------------------------------------------------------------------
static CalLinkState   s_link = {};

void calendarLinkStart() { s_linkReq = true; }

void calendarLinkState(CalLinkState& out) {
  lockTake();
  out = s_link;
  lockGive();
}

static void linkSet(CalLinkPhase ph, const char* code, const char* url,
                    const char* msg) {
  lockTake();
  s_link.phase = ph;
  strlcpy(s_link.code, code ? code : "", sizeof(s_link.code));
  strlcpy(s_link.url, url ? url : "", sizeof(s_link.url));
  strlcpy(s_link.msg, msg ? msg : "", sizeof(s_link.msg));
  lockGive();
}

// Runs on the calendar task, synchronously — a link attempt takes as long as
// the user takes to type the code, and the task has nothing better to do while
// it waits. Fifteen minutes is Microsoft's own expiry on the code.
static void runLinkFlow(const String& cid, uint16_t timeoutMs) {
  linkSet(CAL_LINK_STARTING, nullptr, nullptr, nullptr);

  String body = "client_id=";
  urlEnc(cid.c_str(), body);
  body += "&scope=Calendars.Read%20offline_access";

  String raw;
  NetFetchResult res = netFetchToString(
      MS_DEVICE_URL, true, "Content-Type: application/x-www-form-urlencoded",
      (const uint8_t*)body.c_str(), (uint16_t)body.length(), raw, 4096, timeoutMs);
  if (!res.ok || res.status != 200) {
    linkSet(CAL_LINK_FAILED, nullptr, nullptr,
            res.ok ? "Microsoft refused - check the client ID" : res.error);
    return;
  }

  char devCode[420];        // device_code is long; it never leaves this stack
  uint32_t interval = 5, expires = 900;
  {
    JsonDocument doc;
    if (deserializeJson(doc, raw)) { linkSet(CAL_LINK_FAILED, 0, 0, "bad JSON"); return; }
    const char* dc = doc["device_code"] | "";
    const char* uc = doc["user_code"] | "";
    const char* vu = doc["verification_uri"] | "microsoft.com/devicelogin";
    if (!dc[0] || !uc[0]) {
      linkSet(CAL_LINK_FAILED, 0, 0,
              (const char*)(doc["error_description"] | "no code returned"));
      return;
    }
    strlcpy(devCode, dc, sizeof(devCode));
    interval = doc["interval"] | 5;
    expires = doc["expires_in"] | 900;
    // Strip the scheme: the screen is 240 px wide and everyone knows it is a URL.
    if (!strncmp(vu, "https://", 8)) vu += 8;
    linkSet(CAL_LINK_CODE, uc, vu, nullptr);
  }

  String poll = "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"
                "&client_id=";
  urlEnc(cid.c_str(), poll);
  poll += "&device_code=";
  poll += devCode;          // opaque base64url; nothing in it needs escaping

  const uint32_t deadline = millis() + expires * 1000UL;
  while ((int32_t)(millis() - deadline) < 0) {
    vTaskDelay(pdMS_TO_TICKS(interval * 1000));

    raw = "";
    res = netFetchToString(MS_TOKEN_URL, true,
                           "Content-Type: application/x-www-form-urlencoded",
                           (const uint8_t*)poll.c_str(), (uint16_t)poll.length(),
                           raw, 8192, timeoutMs);
    if (!res.ok) continue;                       // transient; the code still stands

    JsonDocument doc;
    if (deserializeJson(doc, raw)) continue;
    const char* err = doc["error"] | "";
    if (!strcmp(err, "authorization_pending")) continue;
    if (!strcmp(err, "slow_down")) { interval += 5; continue; }
    if (err[0]) {
      linkSet(CAL_LINK_FAILED, 0, 0,
              (const char*)(doc["error_description"] | err));
      return;
    }
    const char* rt = doc["refresh_token"] | "";
    if (!rt[0]) { linkSet(CAL_LINK_FAILED, 0, 0, "no refresh token granted"); return; }

    // Delivered exactly like a rotated token: main.cpp persists it, and the
    // epoch bump from that save restarts the poll with the new credentials.
    lockTake();
    s_rotatedToken = rt;
    lockGive();
    linkSet(CAL_LINK_DONE, nullptr, nullptr, nullptr);
    return;
  }
  linkSet(CAL_LINK_FAILED, 0, 0, "the code expired before it was entered");
}

#endif  // WITH_CALENDAR
