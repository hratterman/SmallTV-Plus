// smalltv-plus — custom firmware for the GeekMagic SmallTV family
//
// Three features, each a self-contained DisplayMode (see Mode.h), picked in the
// web UI and dispatched from the registry below:
//   - Ticker (features/ticker):  stock/crypto price, % change, sparkline.
//   - Usage  (features/usage):   Claude 5h/7d usage bars + animated mascot.
//   - Radar  (features/radar):   live ADS-B plane radar (compiled in when WITH_RADAR).
// Shared plumbing (WiFi, web UI, OTA, display core, settings) lives at src root.
//
// License: WTFPL
#include <Arduino.h>
#include "Platform.h"
#include "config.h"
#include "Settings.h"
#include "Net.h"
#include "Gfx.h"
#include "WebPortal.h"
#include "OtaUpdate.h"
#include "Mode.h"
#include "Clock.h"
#include "Notify.h"
#include <ArduinoJson.h>
#if WITH_CAPTIVE
#include "Captive.h"
#endif
#if WITH_TETHER
#include "Tether.h"
#endif

#if WITH_TICKER
#include "TickerMode.h"
#endif
#if WITH_USAGE
#include "UsageMode.h"
#endif
#if WITH_RADAR
#include "RadarMode.h"
#endif
#if WITH_MINER
#include "MinerMode.h"
#endif
#if WITH_CLOCK
#include "ClockMode.h"
#endif
#if WITH_SPOTIFY
#include "SpotifyMode.h"
#include "SpotifyClient.h"
#endif
#if WITH_AMBIENT
#include "AmbientMode.h"
#endif
#if WITH_GAME
#include "BlackjackMode.h"
#endif
#if WITH_CALENDAR
#include "CalendarMode.h"
#include "CalendarClient.h"
#endif
#if WITH_TICKER
#include "StockClient.h"
#endif
#if WITH_WEATHER
#include "WeatherMode.h"
#include "WeatherClient.h"
#include "RainRadarClient.h"
#endif
#if HAS_TOUCH
#include "Touch.h"
#endif

// ---- mode registry --------------------------------------------------------
// The compiled-in features, in display order. main.cpp holds no per-feature
// state of its own — each mode owns its fetch/render/dirty tracking.
static DisplayMode* kModes[] = {
#if WITH_TICKER
  &g_tickerMode,
#endif
#if WITH_USAGE
  &g_usageMode,
#endif
#if WITH_RADAR
  &g_radarMode,
#endif
#if WITH_MINER
  &g_minerMode,
#endif
#if WITH_CLOCK
  &g_clockMode,
#endif
#if WITH_SPOTIFY
  &g_spotifyMode,
#endif
#if WITH_AMBIENT
  &g_ambientMode,
#endif
#if WITH_GAME
  &g_blackjackMode,
#endif
#if WITH_CALENDAR
  &g_calendarMode,
#endif
#if WITH_WEATHER
  &g_weatherMode,
#endif
};
static const size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

#if HAS_TOUCH
// Touch state. Both are deliberately runtime-only: a tap changing the mode or
// blanking the screen should not rewrite flash, and a reboot returns the cube to
// whatever the web UI has saved.
static int8_t g_modeOverride = -1;    // index into kModes; -1 = follow settings
static bool   g_displayOff   = false;
#endif

// ---- carousel -------------------------------------------------------------
// MODE_CAROUSEL rotates through the ticked features. Switches call wake() on
// the incoming mode: repaint from cached data, no refetch.
static size_t   g_carIdx = 0;
static uint32_t g_carSwitch = 0;

// Advance g_carIdx to the next ticked mode (stays put if none other is ticked).
static void carouselNext(const Settings& s) {
  for (size_t hop = 1; hop <= kModeCount; hop++) {
    size_t cand = (g_carIdx + hop) % kModeCount;
    if (!s.carouselHas(kModes[cand]->modeConst())) continue;
    if (cand != g_carIdx) {
      g_carIdx = cand;
      kModes[cand]->wake(s);
    }
    return;
  }
}

#if WITH_SPOTIFY
// Index of the Spotify mode in the registry, resolved once.
static int spotifyIdx() {
  for (size_t i = 0; i < kModeCount; i++)
    if (kModes[i]->modeConst() == MODE_SPOTIFY) return (int)i;
  return -1;
}
#endif

static DisplayMode* activeMode(const Settings& s) {
#if WITH_SPOTIFY
  // Music takes the screen while it is actually playing, then hands it back.
  // Polling still has to happen when another mode is showing, which is why
  // spotifyService() also runs from the loop below.
  if (s.spotify.autoShow && spotifyIsPlaying(s)) {
    const int i = spotifyIdx();
    if (i >= 0) return kModes[i];
  }
#endif
#if HAS_TOUCH
  // A tap parks the carousel on one mode until the next tap.
  if (g_modeOverride >= 0 && (size_t)g_modeOverride < kModeCount)
    return kModes[g_modeOverride];
#endif
  if (s.mode == MODE_CAROUSEL && kModeCount > 0) {
    if (g_carSwitch == 0) g_carSwitch = millis();
    if (!s.carouselHas(kModes[g_carIdx]->modeConst())) carouselNext(s);   // settings changed
    // A mode in the middle of something keeps the screen; the dwell timer
    // restarts so it gets a full turn once it lets go.
    if (kModes[g_carIdx]->holdsScreen()) {
      g_carSwitch = millis();
      return kModes[g_carIdx];
    }
    uint16_t dwell = kModes[g_carIdx]->dwellSec(s);
    if (!dwell) dwell = s.carouselSec;
    if (millis() - g_carSwitch >= (uint32_t)dwell * 1000UL) {
      g_carSwitch = millis();
      carouselNext(s);
    }
    return kModes[g_carIdx];
  }
  for (size_t i = 0; i < kModeCount; i++)
    if (kModes[i]->modeConst() == s.mode) return kModes[i];
  return kModeCount ? kModes[0] : nullptr;   // fall back to the first compiled mode
}

// Night mining: while the night window is active, the cube goes fully dark and
// does nothing but hash. A tap wakes the screen for a while so you can look at
// it without waiting for morning.
static uint32_t g_nightWakeUntil = 0;

// Longest single pass through loop(), in ms, since it was last read. Anything
// animating shows a stall as a freeze, and this says whether one happened and
// how big without needing a serial cable to find out.
static uint32_t g_loopMaxMs = 0;
static uint32_t g_loopStart = 0;

uint32_t appLoopMaxMs() {
  const uint32_t v = g_loopMaxMs;
  g_loopMaxMs = 0;               // read-and-reset: each poll reports its own window
  return v;
}

static bool nightMiningActive(const Settings& s) {
  if (!s.clock.nightMining || !clockNightActive()) return false;
  if (g_nightWakeUntil) {
    if ((int32_t)(millis() - g_nightWakeUntil) < 0) return false;
    g_nightWakeUntil = 0;   // expired; clear it so the millis() wrap can't revive it
  }
  return true;
}

static Settings g_settings;

#if WITH_TETHER
// The one-frame status the tether page polls: the diagnostics that until now
// could only be read off the panel with a phone camera.
static void tetherStatusFill(String& out) {
  JsonDocument d;
  d["fw"] = FW_VERSION;
  d["up"] = (uint32_t)(millis() / 1000);
  d["heap"] = (uint32_t)ESP.getFreeHeap();
  d["blk"] = (uint32_t)platformMaxFreeBlock();
  d["mode"] = g_settings.mode;
#if WITH_TICKER
  if (stocksNote()[0]) d["ticker"] = stocksNote();
#endif
#if WITH_WEATHER
  {
    WeatherData w;
    weatherSnapshot(w);
    if (w.error && w.errMsg[0]) d["weather"] = w.errMsg;
    else if (w.valid) d["weather"] = "ok";
    d["radar"] = rainRadarNote();
  }
#endif
  serializeJson(d, out);
}
#endif

static String   g_resetReason;        // why the chip last reset (diagnostics)
static bool     g_safeMode = false;   // last reset was an exception -> don't re-enter the crash
static char     g_epcStr[16] = "";
static char     g_addrStr[16] = "";
static int  g_lastBr  = -1;      // last effective brightness written (-1 = none yet)
static bool g_lastInv = false;   // ...and the polarity it was written with
static const char* g_brWhy = "manual";   // which rule picked it (diagnostics)
#if HAS_LDR
static uint32_t g_lastAutoBr = 0;
static uint8_t  g_ldrCache   = DEFAULT_BRIGHTNESS;   // last LDR reading (2 s cadence)
#endif

// Single brightness resolver: night mode overrides auto-brightness overrides the
// manual level. Also records *why*, because a dark screen has five possible
// causes and they are indistinguishable from the outside (see /api/status).
static uint8_t appEffectiveBrightness() {
#if HAS_TOUCH
  if (g_displayOff) { g_brWhy = "blanked"; return 0; }   // double-tap wins over the schedule
#endif
  // Fully dark, not merely dim — except for a pushed banner, which is allowed to
  // light the screen to the night level like it would on any other night.
  if (nightMiningActive(g_settings) && !notifyActive()) { g_brWhy = "nightMining"; return 0; }
  if (clockNightActive()) { g_brWhy = "night"; return g_settings.clock.nightLevel; }
#if HAS_LDR
  if (g_settings.autoBrightness) {
    if (millis() - g_lastAutoBr > 2000) {
      g_lastAutoBr = millis();
      int raw = analogRead(LDR_PIN);
      g_ldrCache = (uint8_t)constrain(raw * 100 / ADC_MAX, 5, 100);
    }
    g_brWhy = "auto";
    return g_ldrCache;
  }
#endif
  g_brWhy = "manual";
  return g_settings.brightness;
}

// The PWM write is skipped when nothing changed, so the cache has to cover
// every input to it. It used to hold only the percentage, which meant flipping
// "backlight is active-low" was a no-op until some *other* thing moved the
// number — the setting appeared not to work, then took effect later out of
// nowhere. Both inputs, or the cache lies.
void appApplyBrightness() {
  const uint8_t t = appEffectiveBrightness();
  const bool inv = g_settings.backlightInverted;
  if ((int)t == g_lastBr && inv == g_lastInv) return;
  g_lastBr = t;
  g_lastInv = inv;
  gfxSetBrightness(t, inv);
}

// For /api/status: the level actually on the panel and the rule that chose it.
uint8_t     appBrightnessLevel() { return (uint8_t)(g_lastBr < 0 ? 0 : g_lastBr); }
const char* appBrightnessWhy()   { return g_brWhy; }

// Exposed to the web portal (/api/status) so the last reset reason is visible.
const char* appResetReason() { return g_resetReason.c_str(); }

// Called by the web portal after settings are applied: re-init every mode and
// force a fresh repaint so a mode/URL/symbol change takes effect immediately.
void appInvalidate() {
  for (size_t i = 0; i < kModeCount; i++) kModes[i]->invalidate(g_settings);
#if HAS_TOUCH
  touchInvalidate(g_settings);
#endif
}

// Step to the next *included* mode: the same ticks that govern the carousel, so
// unticking a feature removes it from the tap cycle too. Stepping starts from
// whatever is on the glass right now rather than from the last tap, so the first
// tap after a carousel switch goes where you expect.
//
// Exposed because a mode that captures the tap still needs a way to hand the
// screen back — its long-press calls this.
void appNextMode() {
#if HAS_TOUCH
  if (!kModeCount) return;
  size_t from = (g_modeOverride < 0) ? 0 : (size_t)g_modeOverride;
  const DisplayMode* cur = activeMode(g_settings);
  for (size_t i = 0; i < kModeCount; i++)
    if (kModes[i] == cur) { from = i; break; }
  for (size_t hop = 1; hop <= kModeCount; hop++) {
    const size_t cand = (from + hop) % kModeCount;
    if (!g_settings.carouselHas(kModes[cand]->modeConst())) continue;
    g_modeOverride = (int8_t)cand;
    kModes[cand]->wake(g_settings);
    return;
  }
  // Nothing ticked at all: don't leave the tap dead, just step anyway.
  g_modeOverride = (int8_t)((from + 1) % kModeCount);
  kModes[g_modeOverride]->wake(g_settings);
#endif
}

#if HAS_TOUCH
// One grammar, every mode. Tap moves through the modes, double-tap blanks the
// screen, and long-press is handed to whichever mode is showing.
static void appHandleTouch(TouchEvent ev) {
  switch (ev) {
    case TOUCH_TAP:
      // A tap during night mining buys a couple of minutes of screen.
      if (nightMiningActive(g_settings)) {
        g_nightWakeUntil = millis() + 120000UL;
        appApplyBrightness();
        DisplayMode* nm = activeMode(g_settings);
        if (nm) nm->wake(g_settings);
        break;
      }
      if (g_displayOff) { g_displayOff = false; appApplyBrightness(); break; }
      if (notifyActive()) { notifyDismiss(); break; }   // tap clears a banner
      {
        // An interactive mode takes the tap for itself — the pad is its control,
        // not navigation — and offers its long-press as the way back out.
        DisplayMode* cur = activeMode(g_settings);
        if (cur && cur->wantsTap()) { cur->onTap(g_settings); break; }
      }
      appNextMode();
      break;

    case TOUCH_DOUBLE:
      g_displayOff = !g_displayOff;
      appApplyBrightness();
      if (!g_displayOff) {
        DisplayMode* m = activeMode(g_settings);
        if (m) m->wake(g_settings);    // repaint what was on screen
      }
      break;

    case TOUCH_LONG: {
      DisplayMode* m = activeMode(g_settings);
      if (m) m->onContextAction(g_settings);
      break;
    }
    default: break;
  }
}
#endif

static void bootProgress(const char* msg) {
  gfxBoot("SmallTV", msg);
}

void setup() {
#if WITH_TETHER
  // The default RX buffer is 256 bytes. The tether pushes multi-KB HTTP bodies
  // down this port in one continuous burst, and while the requester does pump
  // continuously, 256 bytes is only 22 ms of slack at 115200 — one unlucky
  // scheduler pause from dropped bytes that surface as CRC-rejected frames.
  // 4 KB makes the slack a third of a second.
  Serial.setRxBufferSize(4096);
#endif
  Serial.begin(115200);
  Serial.println();
  Serial.println(FW_NAME " " FW_VERSION);

  // Capture why we (re)booted. On a reboot loop this is the key clue, and the
  // device's UART isn't exposed — so we also show it on screen below. On the
  // ESP8266 we also keep the crash PC (epc1) for addr2line decoding; the
  // ESP32-C2 (RISC-V) doesn't expose it, so epc/addr come back empty there.
  PlatformReset pr = platformResetInfo();
  Serial.print("[boot] reset reason: ");
  Serial.println(pr.reason);

  if (pr.wasCrash) {
    g_safeMode = true;                   // crashed last boot -> stay out of the crash path
    strlcpy(g_epcStr,  pr.epc,  sizeof(g_epcStr));
    strlcpy(g_addrStr, pr.addr, sizeof(g_addrStr));
    char rich[80];
    snprintf(rich, sizeof(rich), "%s epc %s addr %s", pr.reason.c_str(),
             g_epcStr[0] ? g_epcStr : "-", g_addrStr[0] ? g_addrStr : "-");
    g_resetReason = rich;
  } else {
    g_resetReason = pr.reason;
  }

  Serial.println("[boot] settings");
  settingsBegin();
  loadSettings(g_settings);

  Serial.println("[boot] display");
  gfxBegin(g_settings);
  gfxBoot(g_safeMode ? "Crashed" : "SmallTV", FW_VERSION);

  Serial.println("[boot] net");
  netBegin(g_settings, bootProgress);
  // Arm SNTP now that WiFi (STA) is up — but only if night mode is enabled, so a
  // ticker-only device doesn't pay the SNTP heap cost (which can starve the cash.ch
  // TLS handshake on the ESP8266). clockReapply arms it iff needed. Skipped after a
  // crash so a fault in here can't boot-loop before the web server starts (the
  // device then comes up in safe mode, OTA-recoverable, instead of needing UART).
  if (!g_safeMode) clockReapply(g_settings);

  // A GitHub update queued from the web UI runs now, before the features claim
  // the heap (the download needs a 16 KB TLS buffer that only fits at boot).
  // On success it reboots into the new image; a no-op stub on the ESP32 targets.
  if (otaBootRequested()) {
    Serial.println("[boot] github update");
    gfxBoot("SmallTV", "updating...");
    otaBootUpdate(g_settings);
    gfxBoot("SmallTV", "update failed");   // still here -> failed; details in the web UI
    delay(1200);
  }

#if WITH_CAPTIVE
  captiveBegin(g_settings);
#endif
#if WITH_TETHER
  tetherBegin();
  // The tether page doubles as the settings UI, because on a network the cube
  // could not join it is the only thing that can reach it at all. Same JSON the
  // web portal uses, same apply path — a second contract here would be a second
  // thing to keep in step, and this codebase has paid for that mistake enough.
#if WITH_CALENDAR
  // A .ics file dropped on the tether page: the one calendar form that always
  // crosses the cable, because browsers may download what scripts may not read.
  tetherOnIcs(calendarImportFeed, calendarImportDone);
#endif
#if WITH_TETHER
  tetherOnOta(otaCableBegin, otaCableWrite, otaCableEnd);
  tetherOnStatus(tetherStatusFill);
#endif
  tetherOnConfig(
      [](String& out) {
        JsonDocument doc;
        JsonObject root = doc.to<JsonObject>();
        settingsToJson(g_settings, root, /*includeSecrets=*/false);
        serializeJson(doc, out);
      },
      [](const String& json) -> const char* {
        JsonDocument doc;
        if (deserializeJson(doc, json)) return "settings were not valid JSON";
        settingsApplyJson(g_settings, doc.as<JsonObjectConst>());
        if (!saveSettings(g_settings)) return "could not write config.json";
        clockReapply(g_settings);
        appApplyBrightness();
        gfxSetRotation(g_settings.rotation);
        appInvalidate();
        return nullptr;
      });
#endif

  Serial.println("[boot] web");
  webPortalBegin(g_settings);

#if HAS_TOUCH
  touchBegin(g_settings);
#endif

  Serial.println("[boot] modes");
  for (size_t i = 0; i < kModeCount; i++) kModes[i]->begin(g_settings);
  Serial.println("[boot] done");

  if (netMode() == NET_AP) {
    gfxApInfo(g_settings.apSsid.c_str(), g_settings.apPass.c_str(), netIP().c_str());
  } else if (g_safeMode) {
    // Last boot crashed: show the crash address (persistent) and keep the web
    // server up for OTA recovery — don't enter the render path that crashed.
    gfxCrash(g_epcStr, g_addrStr, netIP().c_str());
  } else {
    // Show which network we joined and how to reach the web UI, long enough to read.
    gfxStaInfo(netSSID().c_str(), netIP().c_str(), g_settings.hostname.c_str());
    delay(3500);
  }
}

void loop() {
  {
    const uint32_t now = millis();
    if (g_loopStart) {
      const uint32_t took = now - g_loopStart;
      if (took > g_loopMaxMs) g_loopMaxMs = took;
    }
    g_loopStart = now;
  }
  netLoop();
  webPortalLoop();
#if WITH_TETHER
  // Before the AP/safe-mode returns below: a cube that cannot join any network
  // is exactly the one most likely to be tethered, and it still needs the link
  // pumped to find that out.
  tetherService();
#endif

  if (webPortalRebootDue()) {
    delay(120);
    ESP.restart();
  }

  if (g_safeMode) {
    delay(5);
    return;  // crashed last boot: web UI stays up for OTA recovery, no rendering
  }

  // Setup mode holds the screen on the join-the-hotspot card and runs nothing
  // else — right when there is no connection, and wrong the moment a cable is
  // supplying one. A tethered cube has internet; it should behave like it,
  // whatever its radio is doing.
  {
#if WITH_TETHER
    const bool tethered = tetherActive();
#else
    const bool tethered = false;
#endif
    static bool s_ranTethered = false;
    static uint32_t s_tetherLastUp = 0;
    if (tethered) s_tetherLastUp = millis();
    // Once the cable has carried this boot, a gap has to PERSIST before the
    // setup card comes back. Hosts pause all the time - a backgrounded tab, a
    // laptop dozing - and flashing the join-the-hotspot card over a working
    // screen on every hiccup made the cube look broken. Through a short gap
    // the modes keep rendering from cache; a full minute of silence means the
    // cable is genuinely gone and the card is genuinely the right screen.
    const bool holdover =
        s_ranTethered && (uint32_t)(millis() - s_tetherLastUp) < 60000UL;
    if (netMode() == NET_AP && !tethered && !holdover) {
      // Coming back from a tether that has gone away: put the setup card back,
      // otherwise the last rendered frame sits there looking operational.
      if (s_ranTethered) {
        s_ranTethered = false;
        gfxApInfo(g_settings.apSsid.c_str(), g_settings.apPass.c_str(), netIP().c_str());
      }
      delay(5);
      return;
    }
    if (netMode() == NET_AP && tethered && !s_ranTethered) {
      s_ranTethered = true;
      Serial.println("[tether] no WiFi, but the cable works - running normally");
      appInvalidate();     // the setup card is on the glass; repaint over it
    }
  }

#if WITH_CAPTIVE
  // Associated is not online. Probe for a real route out and, behind a portal,
  // try to accept it. Self-paced: a few seconds after boot, then every 30 s
  // while stuck and every 5 minutes once through.
  captiveService(g_settings);
  // Nowhere to reach us and nowhere for us to go: put our own AP back up so
  // there is still a way to change the settings.
  if (captiveStuck()) netStartApAlongside();
#endif

  // --- STA mode: the active feature fetches + renders itself ---

  // Night-mode state machine (NTP-trust gate), then apply the effective brightness
  // (night override / auto-brightness / manual level).
  clockService(g_settings);
  appApplyBrightness();

#if HAS_TOUCH
  {
    TouchEvent ev = touchService(g_settings);
    if (ev != TOUCH_NONE) appHandleTouch(ev);
  }
  if (g_displayOff) { delay(5); return; }   // screen blanked; nothing to draw
#endif

  // Night mining: dark and silent, every spare cycle to the miner. The web
  // server stays up (OTA recovery, and /api/status still reports the hashrate),
  // and touch stays live so a tap can wake the screen — everything else stops.
  // A pushed notification still gets through, since that is its whole point.
  if (nightMiningActive(g_settings) && !notifyActive()) {
    delay(5);
    return;
  }

#if WITH_SPOTIFY
  // A no-op now: the poll lives on its own task so it can neither block a frame
  // nor need permission from whatever is on screen.
  spotifyService(g_settings);
#endif

#if WITH_CALENDAR
  // The poll lives on its own task (calendarInit). What the loop does is
  // persist Microsoft's rotated refresh tokens — without this a reboot months
  // from now would hold a token the provider has since retired.
  {
    static uint32_t s_calChk = 0;
    if (millis() - s_calChk > 5000) {
      s_calChk = millis();
      String rot = calendarTakeRotatedToken();
      if (rot.length()) {
        g_settings.calendar.refreshToken = rot;
        if (saveSettings(g_settings))
          Serial.println("[calendar] stored the rotated refresh token");
      }
    }
  }
  // A banner a few minutes before an event, whatever page is showing.
  calendarReminderService(g_settings);
#endif

#if WITH_WEATHER
  // The weather task cannot draw; it leaves a note when a storm newly enters
  // the forecast, and the loop turns it into a banner when the screen is free.
  if (!notifyActive()) {
    char wxAlert[48];
    if (weatherTakeAlert(wxAlert, sizeof(wxAlert)))
      notifyShow(wxAlert, 12, 0xFFE0);   // the storm icon's bolt yellow
  }
#endif

  // A pushed banner owns the screen while it lasts, then the mode repaints.
  static bool s_hadNotify = false;
  // The one place the Typeface setting turns into draw behavior: everything
  // below (notifications included) renders under whatever is latched here.
  gfxTypeSans(g_settings.numFont == NUM_FONT_SANS);

  if (notifyActive()) {
    notifyService();
    s_hadNotify = true;
    delay(5);
    return;
  }
  if (s_hadNotify) {
    s_hadNotify = false;
    DisplayMode* back = activeMode(g_settings);
    if (back) back->wake(g_settings);
  }

  // Whenever the active mode changes for any reason — carousel, a tap, or
  // Spotify pulling focus while music plays — the incoming mode repaints from
  // its cached data. Without this a mode that thinks its panel is already
  // correct would leave the previous mode's screen up.
  DisplayMode* m = activeMode(g_settings);
  static DisplayMode* s_lastMode = nullptr;
  if (m && m != s_lastMode) {
    s_lastMode = m;
    m->wake(g_settings);
  }
  if (m) m->service(g_settings);

  delay(5);
}
