#include "WeatherMode.h"
#if WITH_WEATHER

#include <Arduino_GFX_Library.h>
#include <math.h>
#include "Gfx.h"
#include "WeatherClient.h"
#include "RainRadarClient.h"

WeatherMode g_weatherMode;

// Palette. Weather is one of the few screens with licence for colour.
#define C_SUN    0xFEA0   // amber disc and rays
#define C_MOON   0xE71C   // pale moonlight
#define C_CLOUDM 0xAD75   // cloud body
#define C_CLOUDD 0x632C   // cloud in the overcast/fog icon's shadow
#define C_RAINB  0x34BF   // rain drops
#define C_BOLT   0xFFE0   // lightning
#define C_FOGL   0x8410   // fog bars
#define C_DIMTX  0x8410
#define C_FAINT  0x4208

// ---- icons ----------------------------------------------------------------
// All primitive-drawn inside a box centred on (cx,cy) with half-size r, so
// the same function draws the 34 px hero icon and the 17 px forecast ones.
// tools/render_faces.py transcribes these exact expressions for the preview
// sheet — change geometry here and there together.

static void wxSun(Arduino_GFX* g, int cx, int cy, int r, uint16_t col) {
  const int rr = r * 5 / 8;
  g->fillCircle(cx, cy, rr - 3, col);
  for (int i = 0; i < 8; i++) {
    static const int8_t dx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    static const int8_t dy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    const bool diag = dx[i] && dy[i];
    const int a = rr, b = diag ? r * 3 / 4 : r;   // diagonals shortened
    g->drawLine(cx + dx[i] * a, cy + dy[i] * a, cx + dx[i] * b, cy + dy[i] * b, col);
    g->drawLine(cx + dx[i] * a + (dy[i] ? 1 : 0), cy + dy[i] * a + (dx[i] ? 1 : 0),
                cx + dx[i] * b + (dy[i] ? 1 : 0), cy + dy[i] * b + (dx[i] ? 1 : 0), col);
  }
}

static void wxMoon(Arduino_GFX* g, int cx, int cy, int r) {
  g->fillCircle(cx, cy, r * 5 / 8, C_MOON);
  g->fillCircle(cx + r / 3, cy - r / 4, r / 2, C_BLACK);
}

static void wxCloud(Arduino_GFX* g, int cx, int cy, int r, uint16_t col) {
  const int by = cy + r / 4;
  g->fillCircle(cx - r / 2, by - r / 4, r / 2 + 1, col);
  g->fillCircle(cx + r / 8, by - r / 2, r * 5 / 8, col);
  g->fillRoundRect(cx - r + 2, by - r / 4, 2 * r - 4, r / 2, r / 4, col);
}

void gfxWxIcon(Arduino_GFX* g, uint8_t klass, bool day, int cx, int cy, int r) {
  switch ((WxClass)klass) {
    case WX_CLEAR:
      if (day) wxSun(g, cx, cy, r, C_SUN);
      else     wxMoon(g, cx, cy, r);
      break;
    case WX_PARTLY:
      wxSun(g, cx - r / 3, cy - r / 3, r * 7 / 12, C_SUN);
      wxCloud(g, cx + r / 8, cy + r / 6, r * 3 / 4, C_CLOUDM);
      break;
    case WX_CLOUD:
      wxCloud(g, cx, cy - r / 6, r, C_CLOUDD);
      wxCloud(g, cx - r / 8, cy, r, C_CLOUDM);
      break;
    case WX_FOG: {
      wxCloud(g, cx, cy - r / 3, r * 7 / 8, C_CLOUDM);
      for (int i = 0; i < 3; i++) {
        const int y = cy + r / 4 + i * (r / 4 + 1);
        g->fillRect(cx - r + 2 + (i % 2) * (r / 4), y, r * 3 / 2, 2, C_FOGL);
      }
      break;
    }
    case WX_RAIN: {
      wxCloud(g, cx, cy - r / 3, r * 7 / 8, C_CLOUDM);
      for (int i = 0; i < 3; i++) {
        const int x = cx - r / 2 + i * (r / 2);
        const int y = cy + r / 4;
        g->drawLine(x, y, x - r / 6, y + r / 3, C_RAINB);
        g->drawLine(x + 1, y, x + 1 - r / 6, y + r / 3, C_RAINB);
      }
      break;
    }
    case WX_SNOW: {
      wxCloud(g, cx, cy - r / 3, r * 7 / 8, C_CLOUDM);
      const int fr = r > 20 ? 2 : 1;
      for (int i = 0; i < 3; i++)
        g->fillCircle(cx - r / 2 + i * (r / 2), cy + r / 3 + (i % 2) * (r / 6), fr, C_WHITE);
      break;
    }
    case WX_STORM: {
      wxCloud(g, cx, cy - r / 3, r * 7 / 8, C_CLOUDD);
      // The classic jagged bolt: a 7-vertex zigzag outline (percent
      // coordinates in the box below), filled as three triangles - the two
      // halves of the upper band, then the lower spike whose right corner
      // juts past the band to make the jag.
      const int x0 = cx - r / 2, y0 = cy - r / 8, w = r, h = r * 7 / 8;
#define WXPT(px_, py_) x0 + (px_)*w / 100, y0 + (py_)*h / 100
      g->fillTriangle(WXPT(60, 0), WXPT(20, 55), WXPT(45, 55), C_BOLT);
      g->fillTriangle(WXPT(60, 0), WXPT(45, 55), WXPT(75, 0), C_BOLT);
      g->fillTriangle(WXPT(52, 40), WXPT(80, 40), WXPT(30, 100), C_BOLT);
#undef WXPT
      break;
    }
  }
}

// ---- layout ---------------------------------------------------------------
static const int HERO_CX = 52,  HERO_CY = 62, HERO_R = 34;
static const int TEMP_X  = 104, TEMP_Y  = 28;
static const int HL_Y    = 88;
static const int DIV_Y   = 118;
static const int FC_LBL_Y = 128, FC_CY = 162, FC_R = 17;
static const int FC_HI_Y = 186, FC_LO_Y = 206;
static const int FOOT_Y  = 228;

static void drawTemp(Arduino_GFX* gfx, const Settings& s, float t) {
  char num[8];
  snprintf(num, sizeof(num), "%ld", lroundf(t));
  int x = TEMP_X;
  if (s.numFont == NUM_FONT_SANS && gfxNumEligible(num)) {
    int face = gfxNumFace(num, 100);
    const int w = gfxNumFaceW(num, face);
    gfxNumFaceDraw(x, TEMP_Y, num, face, C_WHITE);
    x += w;
  } else {
    const uint8_t sz = gfxFitSize(num, 100, 5);
    gfx->setTextSize(sz);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(x, TEMP_Y);
    gfx->print(num);
    x += gfxTextW(num, sz);
  }
  // The degree mark is drawn, not typed: neither font carries one.
  gfx->drawCircle(x + 7, TEMP_Y + 6, 4, C_WHITE);
  gfx->drawCircle(x + 7, TEMP_Y + 6, 3, C_WHITE);
  gfxLabel(x + 16, TEMP_Y + 2, s.weather.unitsF ? "F" : "C", 2, C_DIMTX);
}

void WeatherMode::render(const Settings& s, const WeatherData& w) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);

  if (s.weather.lat == 0.0f && s.weather.lon == 0.0f) {
    gfxDrawCentered("no location set", 96, 2, C_WHITE);
    gfxDrawCentered("set latitude/longitude", 126, 1, C_DIMTX);
    gfxDrawCentered("in the web UI's Weather tab", 140, 1, C_DIMTX);
    return;
  }
  if (!w.valid) {
    gfxDrawCentered("WEATHER", 80, 3, C_WHITE);
    gfxDrawCentered(w.error ? "fetch error" : "fetching...", 124, 2, C_DIMTX);
    // The reason, split over two size-1 lines - the message carries the
    // resolved IP and heap details, and clipping those defeats its purpose.
    if (w.error && w.errMsg[0]) {
      const char* n = w.errMsg;
      const size_t len = strlen(n);
      if (len <= 38) {
        gfxDrawCentered(n, 152, 1, C_FAINT);
      } else {
        char l1[40];
        size_t cut = 38;
        while (cut > 20 && n[cut] != ' ') cut--;
        if (n[cut] != ' ') cut = 38;
        memcpy(l1, n, cut);
        l1[cut] = 0;
        gfxDrawCentered(l1, 152, 1, C_FAINT);
        gfxDrawCentered(n + cut + (n[cut] == ' ' ? 1 : 0), 166, 1, C_FAINT);
      }
    }
    return;
  }

  // Current conditions: icon + big temp + today's range.
  gfxWxIcon(gfx, wxClass(w.curCode), w.day, HERO_CX, HERO_CY, HERO_R);
  drawTemp(gfx, s, w.curTemp);
  {
    char hi[12], lo[12];
    snprintf(hi, sizeof(hi), "H %.0f", (double)w.hi[0]);
    snprintf(lo, sizeof(lo), "L %.0f", (double)w.lo[0]);
    gfxLabel(TEMP_X, HL_Y, hi, 2, C_WHITE);
    gfxLabel(TEMP_X + gfxLabelW(hi, 2) + 14, HL_Y, lo, 2, C_DIMTX);
  }

  gfx->drawFastHLine(12, DIV_Y, TFT_WIDTH - 24, C_FAINT);

  // Three-day forecast columns.
  static const char* kDay[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  for (int i = 1; i < WX_DAYS; i++) {
    const int cx = 40 + (i - 1) * 80;
    const char* lbl = (w.dow[i] >= 0 && w.dow[i] < 7) ? kDay[w.dow[i]] : "---";
    gfxLabel(cx - gfxLabelW(lbl, 1) / 2, FC_LBL_Y, lbl, 1, C_DIMTX);
    gfxWxIcon(gfx, wxClass(w.code[i]), true, cx, FC_CY, FC_R);
    char t[8];
    snprintf(t, sizeof(t), "%.0f", (double)w.hi[i]);
    gfxLabel(cx - gfxLabelW(t, 2) / 2, FC_HI_Y, t, 2, C_WHITE);
    snprintf(t, sizeof(t), "%.0f", (double)w.lo[i]);
    gfxLabel(cx - gfxLabelW(t, 2) / 2, FC_LO_Y, t, 2, C_DIMTX);
  }

  // Footer: age, and a quiet red dot when the last refresh failed. While the
  // radar is enabled but has nothing to show, its status note takes the line
  // every other repaint - the screen is where this cube explains itself, and
  // "radar quiet" versus "radar: heap 82k" versus a fetch error are three
  // different conversations.
  {
    char f[52];
    const char* rn = rainRadarNote();
    if (s.weather.rainRadar && !rainRadarReady() && rn[0] && (footAlt_ = !footAlt_)) {
      strlcpy(f, rn, sizeof(f));
    } else {
      const uint32_t age = (millis() - w.lastOkMs) / 60000UL;
      if (age < 1) strlcpy(f, "just updated", sizeof(f));
      else         snprintf(f, sizeof(f), "updated %lum ago", (unsigned long)age);
    }
    gfxDrawCentered(f, FOOT_Y, 1, C_FAINT);
    if (w.error) gfx->fillCircle(6, 6, 3, C_RED);
  }
}

void WeatherMode::begin(const Settings& s) {
  weatherInit(s);
  needFull_ = true;
  condSince_ = millis();
}

void WeatherMode::invalidate(const Settings& s) {
  weatherInit(s);
  needFull_ = true;
}

void WeatherMode::wake(const Settings& s) {
  (void)s;
  needFull_ = true;
  subRadar_ = false;          // another mode drew; come back on conditions
  condSince_ = millis();
}

uint16_t WeatherMode::dwellSec(const Settings& s) const {
  // One radar pass is ~7 s; give the slot enough room for conditions AND the
  // animation, otherwise the carousel rotates away just as the rain starts.
  return rainRadarReady() ? (uint16_t)(s.carouselSec + 14) : 0;
}

void WeatherMode::onContextAction(Settings& s) {
  (void)s;
  if (!subRadar_ && rainRadarReady()) {
    subRadar_ = true;
    rFrame_ = 0;
    rLoops_ = 0;
    rTick_ = 0;
  }
}

// Frame pacing: a beat per frame, a longer beat on the last one so the loop
// visibly ends before it starts again.
#define RR_FRAME_MS 500
#define RR_HOLD_MS  1400
#define RR_SHOW_LOOPS 2
#define RR_COND_SEC 8

// The whole radar screen is repainted per animation frame from RAM: the dim
// map, the rain cells blended over it, the location dot, then the header and
// timeline bands. ~60 ms of SPI per frame, twice a second.
bool WeatherMode::drawRadarFrame() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return false;
  RainRadarView v;
  if (!rainRadarAcquire(v)) return false;
  if (rFrame_ >= v.frames) rFrame_ = 0;
  rFrames_ = v.frames;
  const uint8_t* g = v.grid[rFrame_];

  uint16_t line[TFT_WIDTH];
  for (int y = 0; y < TFT_HEIGHT; y++) {
    const int ty = y + RR_CROP;
    const uint8_t* mrow = v.map + (size_t)(ty >> 1) * RR_MAP_PX;
    const int gy = ty >> 2;
    for (int x = 0; x < TFT_WIDTH; x++) {
      const int tx = x + RR_CROP;
      uint16_t c = rr332to565(mrow[tx >> 1]);
      const uint8_t n = rrGridGet(g, tx >> 2, gy);
      if (n >= RR_GATE_NIBBLE) c = rrBlend565(c, rrPalette[n]);
      line[x] = c;
    }
    gfx->draw16bitRGBBitmap(0, y, line, TFT_WIDTH, 1);
  }

  // You are here.
  const int mx = (int)v.markerX - RR_CROP, my = (int)v.markerY - RR_CROP;
  if (mx >= 4 && mx < TFT_WIDTH - 4 && my >= 4 && my < TFT_HEIGHT - 4) {
    gfx->drawCircle(mx, my, 4, C_BLACK);
    gfx->drawCircle(mx, my, 3, C_WHITE);
    gfx->fillCircle(mx, my, 1, C_WHITE);
  }

  // Header band: what this is, and when this frame was.
  gfx->fillRect(0, 0, TFT_WIDTH, 14, C_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(C_DIMTX);
  gfx->setCursor(6, 3);
  gfx->print("RADAR");
  char lbl[8];
  const int off = (int)v.minOff[rFrame_];
  if (rFrame_ == v.nowIdx || off == 0) strlcpy(lbl, "now", sizeof(lbl));
  else snprintf(lbl, sizeof(lbl), "%+dm", off);
  gfx->setTextColor(rFrame_ > v.nowIdx ? C_RAINB : C_WHITE);
  gfx->setCursor(TFT_WIDTH - 6 - gfxTextW(lbl, 1), 3);
  gfx->print(lbl);

  // Timeline band: a tick per frame, the observed/forecast divide, a cursor.
  gfx->fillRect(0, TFT_HEIGHT - 16, TFT_WIDTH, 16, C_BLACK);
  gfx->drawFastHLine(RR_TL_X0, RR_TL_Y + 2, RR_TL_W + 1, C_FAINT);
  for (uint8_t i = 0; i < v.frames; i++) {
    const int x = rrTimelineX(i, v.frames);
    gfx->drawFastVLine(x, RR_TL_Y, 5, i > v.nowIdx ? C_RAINB : C_DIMTX);
  }
  const int nx = rrTimelineX(v.nowIdx, v.frames);
  gfx->drawFastVLine(nx, RR_TL_Y - 2, 9, C_WHITE);
  const int cx = rrTimelineX(rFrame_, v.frames);
  gfx->fillRect(cx - 2, RR_TL_Y - 1, 5, 7, C_SUN);

  rainRadarRelease();
  return true;
}

void WeatherMode::service(const Settings& s) {
  const uint32_t now = millis();

  if (subRadar_) {
    // The animation. Each frame holds for a beat, the last for longer; after
    // a couple of loops the conditions screen takes the glass back.
    const uint32_t hold =
        (rFrames_ && rFrame_ + 1 >= rFrames_) ? RR_HOLD_MS : RR_FRAME_MS;
    if (rTick_ && (now - rTick_) < hold) return;
    if (rTick_) {                        // advance past the frame just shown
      rFrame_++;
      if (rFrames_ && rFrame_ >= rFrames_) {
        rFrame_ = 0;
        if (++rLoops_ >= RR_SHOW_LOOPS) {
          subRadar_ = false;
          needFull_ = true;
          condSince_ = now;
        }
      }
    }
    if (subRadar_) {
      rTick_ = now;
      if (!drawRadarFrame()) {           // radar went away: back to conditions
        subRadar_ = false;
        needFull_ = true;
        condSince_ = now;
      } else {
        return;
      }
    }
  }

  // The conditions screen. The fetch lives on its own task; this only decides
  // whether to repaint.
  WeatherData w;
  weatherSnapshot(w);
  bool repaint = needFull_;
  if (w.lastOkMs != renderedOk_ || w.error != renderedErr_) repaint = true;
  // Once a minute for the footer age (and the fetching/error screens).
  if (now - lastDrawMs_ >= 60000UL) repaint = true;
  if (repaint) {
    needFull_ = false;
    renderedOk_ = w.lastOkMs;
    renderedErr_ = w.error;
    lastDrawMs_ = now;
    render(s, w);
  }

  // After the conditions have had their say, the timelapse takes a turn —
  // but only when there is actually something on the radar to show.
  if (w.valid && rainRadarReady() && (now - condSince_) >= RR_COND_SEC * 1000UL) {
    subRadar_ = true;
    rFrame_ = 0;
    rLoops_ = 0;
    rTick_ = 0;
  }
}

#endif  // WITH_WEATHER
