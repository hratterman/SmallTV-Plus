// smalltv-mod — custom firmware for the GeekMagic SmallTV (ESP-12F / ESP8266)
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

static bool carouselHas(const Settings& s, const DisplayMode* m) {
  switch (m->modeConst()) {
    case MODE_STOCKS: return s.carouselTicker;
    case MODE_USAGE:  return s.carouselUsage;
    case MODE_RADAR:  return s.carouselRadar;
    case MODE_MINER:  return s.carouselMiner;
    case MODE_CLOCK:  return s.carouselClock;
    case MODE_SPOTIFY: return s.carouselSpotify;
    case MODE_AMBIENT: return s.carouselAmbient;
    default:          return true;
  }
}

// Advance g_carIdx to the next ticked mode (stays put if none other is ticked).
static void carouselNext(const Settings& s) {
  for (size_t hop = 1; hop <= kModeCount; hop++) {
    size_t cand = (g_carIdx + hop) % kModeCount;
    if (!carouselHas(s, kModes[cand])) continue;
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
    if (!carouselHas(s, kModes[g_carIdx])) carouselNext(s);   // settings changed
    if (millis() - g_carSwitch >= (uint32_t)s.carouselSec * 1000UL) {
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

static bool nightMiningActive(const Settings& s) {
  if (!s.clock.nightMining || !clockNightActive()) return false;
  if (g_nightWakeUntil) {
    if ((int32_t)(millis() - g_nightWakeUntil) < 0) return false;
    g_nightWakeUntil = 0;   // expired; clear it so the millis() wrap can't revive it
  }
  return true;
}

static Settings g_settings;
static String   g_resetReason;        // why the chip last reset (diagnostics)
static bool     g_safeMode = false;   // last reset was an exception -> don't re-enter the crash
static char     g_epcStr[16] = "";
static char     g_addrStr[16] = "";
static int g_lastBr = -1;        // last effective brightness written (-1 = none yet)
#if HAS_LDR
static uint32_t g_lastAutoBr = 0;
static uint8_t  g_ldrCache   = DEFAULT_BRIGHTNESS;   // last LDR reading (2 s cadence)
#endif

// Single brightness resolver: night mode overrides auto-brightness overrides the
// manual level. Only writes the PWM when the effective target changes.
static uint8_t appEffectiveBrightness() {
#if HAS_TOUCH
  if (g_displayOff) return 0;          // double-tap wins over the night schedule
#endif
  // Fully dark, not merely dim — except for a pushed banner, which is allowed to
  // light the screen to the night level like it would on any other night.
  if (nightMiningActive(g_settings) && !notifyActive()) return 0;
  if (clockNightActive()) return g_settings.clock.nightLevel;
#if HAS_LDR
  if (g_settings.autoBrightness) {
    if (millis() - g_lastAutoBr > 2000) {
      g_lastAutoBr = millis();
      int raw = analogRead(LDR_PIN);
      g_ldrCache = (uint8_t)constrain(raw * 100 / ADC_MAX, 5, 100);
    }
    return g_ldrCache;
  }
#endif
  return g_settings.brightness;
}

void appApplyBrightness() {
  uint8_t t = appEffectiveBrightness();
  if ((int)t != g_lastBr) {
    g_lastBr = t;
    gfxSetBrightness(t, g_settings.backlightInverted);
  }
}

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
      // Step to the next *included* mode: the same ticks that govern the
      // carousel, so unticking a feature removes it from the tap cycle too.
      // Stepping starts from whatever is on the glass right now, not from the
      // last tap, so the first tap after a carousel switch goes where you expect.
      if (kModeCount) {
        size_t from = (g_modeOverride < 0) ? 0 : (size_t)g_modeOverride;
        const DisplayMode* cur = activeMode(g_settings);
        for (size_t i = 0; i < kModeCount; i++)
          if (kModes[i] == cur) { from = i; break; }
        bool stepped = false;
        for (size_t hop = 1; hop <= kModeCount && !stepped; hop++) {
          const size_t cand = (from + hop) % kModeCount;
          if (!carouselHas(g_settings, kModes[cand])) continue;
          g_modeOverride = (int8_t)cand;
          kModes[cand]->wake(g_settings);
          stepped = true;
        }
        // Nothing ticked at all: don't leave the tap dead, just step anyway.
        if (!stepped) {
          g_modeOverride = (int8_t)((from + 1) % kModeCount);
          kModes[g_modeOverride]->wake(g_settings);
        }
      }
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
  netLoop();
  webPortalLoop();

  if (webPortalRebootDue()) {
    delay(120);
    ESP.restart();
  }

  if (g_safeMode) {
    delay(5);
    return;  // crashed last boot: web UI stays up for OTA recovery, no rendering
  }

  if (netMode() == NET_AP) {
    delay(5);
    return;  // setup mode: AP info stays on screen
  }

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
  // Poll regardless of what is showing: auto-takeover depends on noticing that
  // playback started while a different mode has the screen.
  spotifyService(g_settings);
#endif

  // A pushed banner owns the screen while it lasts, then the mode repaints.
  static bool s_hadNotify = false;
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
