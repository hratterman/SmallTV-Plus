#include "ClockMode.h"
#if WITH_CLOCK

#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Clock.h"

ClockMode g_clockMode;

#define C_DIM   0xB574
#define C_PANEL 0x18E3

static const char* kDays[7]   = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                 "Thursday", "Friday", "Saturday"};
static const char* kMonths[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

void ClockMode::begin(const Settings& s) { needFull_ = true; }

void ClockMode::invalidate(const Settings& s) {
  needFull_ = true;
  lastMin_ = lastSec_ = lastDay_ = -1;
}

// Long-press flips between 12- and 24-hour without opening the web UI. Runtime
// only, like every other gesture — a reboot returns to the saved preference.
void ClockMode::onContextAction(Settings& s) {
  s.clock.mode12h = !s.clock.mode12h;
  invalidate(s);
}

void ClockMode::render(const Settings& s, bool full) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  struct tm t;
  if (!clockNow(t)) {
    if (full) {
      gfx->fillScreen(C_BLACK);
      gfxDrawCentered("waiting for", 96, 2, C_DIM);
      gfxDrawCentered("network time", 122, 2, C_DIM);
      gfxDrawCentered(s.clock.tz.length() ? s.clock.tz.c_str() : "timezone not set",
                      170, 1, C_DGRAY);
    }
    return;
  }

  if (full) gfx->fillScreen(C_BLACK);

  int hour = t.tm_hour;
  const char* suffix = nullptr;
  if (s.clock.mode12h) {
    suffix = (hour < 12) ? "AM" : "PM";
    hour = hour % 12;
    if (hour == 0) hour = 12;
  }

  // --- big HH:MM, only repainted when the minute turns ---
  if (full || t.tm_min != lastMin_) {
    lastMin_ = t.tm_min;
    char hhmm[8];
    snprintf(hhmm, sizeof(hhmm), s.clock.mode12h ? "%d:%02d" : "%02d:%02d", hour, t.tm_min);

    // Auto-size so both "9:05" and "23:45" fill the width nicely.
    uint8_t sz = gfxFitSize(hhmm, TFT_WIDTH - 24, 8);
    const int th = 8 * sz;
    const int ty = 78 - th / 2;
    gfx->fillRect(0, ty - 4, TFT_WIDTH, th + 8, C_BLACK);
    gfxDrawCentered(hhmm, ty, sz, C_WHITE);

    if (suffix) {
      gfx->setTextSize(2);
      gfx->setTextColor(C_DIM);
      gfx->setCursor(TFT_WIDTH - gfxTextW(suffix, 2) - 10, ty + th - 16);
      gfx->print(suffix);
    }
  }

  // --- seconds bar: a full sweep per minute, cheaper and calmer than digits ---
  if (s.clock.showSeconds && (full || t.tm_sec != lastSec_)) {
    lastSec_ = t.tm_sec;
    const int bx = 20, by = 128, bw = TFT_WIDTH - 40, bh = 6;
    gfx->fillRoundRect(bx, by, bw, bh, bh / 2, C_PANEL);
    const int fw = (int)((long)bw * (t.tm_sec + 1) / 60);
    if (fw >= bh)     gfx->fillRoundRect(bx, by, fw, bh, bh / 2, C_BLUE);
    else if (fw > 0)  gfx->fillRect(bx, by, fw, bh, C_BLUE);
  }

  // --- date, once a day ---
  if (s.clock.showDate && (full || t.tm_mday != lastDay_)) {
    lastDay_ = t.tm_mday;
    char line[40];
    snprintf(line, sizeof(line), "%s %d %s", kDays[t.tm_wday % 7], t.tm_mday,
             kMonths[t.tm_mon % 12]);
    gfx->fillRect(0, 158, TFT_WIDTH, 24, C_BLACK);
    gfxDrawCentered(line, 160, 2, C_DIM);
  }

  // --- footer: timezone, and a warning when the clock has drifted out of trust ---
  if (full) {
    const char* foot = s.clock.tz.length() ? s.clock.tz.c_str() : "UTC";
    gfxDrawCentered(foot, 214, 1, C_DGRAY);
  }
  if (!clockTrusted()) gfx->fillCircle(228, 12, 4, C_YELLOW);
}

void ClockMode::service(const Settings& s) {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (!needFull_ && now - last < 250) return;   // fine enough for a seconds bar
  last = now;
  const bool full = needFull_;
  needFull_ = false;
  render(s, full);
}

#endif  // WITH_CLOCK
