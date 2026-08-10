#include "ClockMode.h"
#if WITH_CLOCK

#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Clock.h"
#include "ClockLayout.h"
#include "ClockFaces.h"
#include "font_clock_sans.h"

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

// ---------------------------------------------------------------------------
// The three faces. Each clears its own band and draws HH:MM plus the AM/PM;
// everything else on the screen is face-independent. All three centre on the
// same vertical middle so switching faces does not move the seconds bar's
// relationship to the time.
// ---------------------------------------------------------------------------
static const int kFaceCenterY = 78;

// Face 2: seven segments, drawn. Ghost segments stay faintly lit like a real
// LCD, which is what makes it read as a clock rather than four stencils.
static void draw7segDigit(Arduino_GFX* gfx, int x, int y, int w, int h, int th,
                          int digit, uint16_t on, uint16_t off) {
  const uint8_t lit = (digit >= 0 && digit <= 9) ? kSevenSeg[digit] : 0;
  for (uint8_t bit = 0; bit < 7; bit++) {
    SegRect r;
    if (!segRect((uint8_t)(1u << bit), w, h, th, r)) continue;
    // The 1px inset keeps neighbouring segments from touching at the corners,
    // which is what sells the seven-segment look.
    gfx->fillRect(x + r.x + 1, y + r.y + 1, r.w - 2, r.h - 2,
                  (lit & (1u << bit)) ? on : off);
  }
}

static void drawDigits(const Settings& s, const char* hhmm, const char* suffix) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const int suffixW = suffix ? clockTextW(2, CLOCK_SUFFIX_SIZE) + CLOCK_SUFFIX_GAP : 0;

  if (s.clock.face == CLOCK_FACE_SANS) {
    // Type set from the baseline, the GFXfont convention. The digits are
    // monospaced by the generator, so the width is arithmetic, not measurement.
    int w = 0;
    for (const char* p = hhmm; *p; p++)
      w += (*p == ':') ? CLOCK_SANS_COLON_ADV : CLOCK_SANS_DIGIT_ADV;
    const int bandTop = kFaceCenterY - (CLOCK_SANS_ASCENT + CLOCK_SANS_DESCENT) / 2 - 4;
    const int bandH   = CLOCK_SANS_ASCENT + CLOCK_SANS_DESCENT + 8;
    gfx->fillRect(0, bandTop, TFT_WIDTH, bandH, C_BLACK);

    int x = (TFT_WIDTH - (w + suffixW)) / 2;
    if (x < 0) x = 0;
    const int baseline = kFaceCenterY + CLOCK_SANS_ASCENT / 2;
    gfx->setFont(&ClockSans);
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(x, baseline);
    gfx->print(hhmm);
    gfx->setFont(nullptr);          // back to the classic font for everyone else

    if (suffix) {
      gfx->setTextSize(CLOCK_SUFFIX_SIZE);
      gfx->setTextColor(C_DIM);
      gfx->setCursor(x + w + CLOCK_SUFFIX_GAP, baseline - 8 * CLOCK_SUFFIX_SIZE);
      gfx->print(suffix);
    }
    return;
  }

  if (s.clock.face == CLOCK_FACE_7SEG) {
    const int th = 9, gap = 8;
    const int h = 92;
    // Four cells and the colon column; the suffix eats into the cell width in
    // 12h mode rather than overhanging the edge.
    const int colonW = th + 4;
    int cw = (TFT_WIDTH - 8 - colonW - 4 * gap - suffixW) / 4;
    if (cw > 48) cw = 48;
    const int total = 4 * cw + colonW + 4 * gap;
    int x = (TFT_WIDTH - (total + suffixW)) / 2;
    const int y = kFaceCenterY - h / 2;
    gfx->fillRect(0, y - 4, TFT_WIDTH, h + 8, C_BLACK);

    // "9:05" has three digits; the leading cell simply stays ghosted.
    int d[4] = {-1, -1, -1, -1};
    const size_t len = strlen(hhmm);            // "H:MM" or "HH:MM"
    if (len == 5) { d[0] = hhmm[0] - '0'; d[1] = hhmm[1] - '0';
                    d[2] = hhmm[3] - '0'; d[3] = hhmm[4] - '0'; }
    else          { d[1] = hhmm[0] - '0';
                    d[2] = hhmm[2] - '0'; d[3] = hhmm[3] - '0'; }

    const uint16_t ghost = 0x10A2;              // barely-there segments
    draw7segDigit(gfx, x, y, cw, h, th, d[0], C_WHITE, ghost); x += cw + gap;
    draw7segDigit(gfx, x, y, cw, h, th, d[1], C_WHITE, ghost); x += cw + gap;
    // The colon: two dots that share the segment stroke.
    gfx->fillRect(x + 2, y + h / 3 - th / 2, th - 2, th - 2, C_WHITE);
    gfx->fillRect(x + 2, y + 2 * h / 3 - th / 2, th - 2, th - 2, C_WHITE);
    x += colonW + gap;
    draw7segDigit(gfx, x, y, cw, h, th, d[2], C_WHITE, ghost); x += cw + gap;
    draw7segDigit(gfx, x, y, cw, h, th, d[3], C_WHITE, ghost); x += cw + gap;

    if (suffix) {
      gfx->setTextSize(CLOCK_SUFFIX_SIZE);
      gfx->setTextColor(C_DIM);
      gfx->setCursor(x + CLOCK_SUFFIX_GAP - gap, y + h - 8 * CLOCK_SUFFIX_SIZE);
      gfx->print(suffix);
    }
    return;
  }

  // Face 0: the scaled pixel font, auto-sized. See ClockLayout.h.
  const ClockLayout L = clockLayout((int)strlen(hhmm), suffix ? 2 : 0,
                                    TFT_WIDTH, kFaceCenterY);
  gfx->fillRect(0, L.digitsY - 4, TFT_WIDTH, L.digitsH + 8, C_BLACK);
  gfx->setTextSize(L.size);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor(L.digitsX, L.digitsY);
  gfx->print(hhmm);

  if (suffix) {
    gfx->setTextSize(CLOCK_SUFFIX_SIZE);
    gfx->setTextColor(C_DIM);
    gfx->setCursor(L.suffixX, L.suffixY);
    gfx->print(suffix);
  }
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
    drawDigits(s, hhmm, suffix);
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
