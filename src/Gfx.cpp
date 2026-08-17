#include "TextFold.h"
#include "Gfx.h"
#include "GfxMarqueeStep.h"
#include "Platform.h"
#include <Arduino_GFX_Library.h>
#include <SPI.h>

// The SmallTV's ST7789 has its CS line tied to GND and only latches SPI in
// **mode 3**. Arduino_GFX's stock Arduino_ST7789 forces SPI_MODE2 on the ESP8266
// (wrong clock edge for this panel), so the controller never initializes and the
// screen stays black even with the backlight on. Subclass begin() to force mode 3
// — matching the known-good GeekMagic community firmwares. (On ESP32 the base
// class already selects mode 3, so the override is harmless there.)
class Arduino_ST7789_SmallTV : public Arduino_ST7789 {
 public:
  using Arduino_ST7789::Arduino_ST7789;   // inherit constructors
  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    _override_datamode = SPI_MODE3;
    return Arduino_TFT::begin(speed);
  }

#if TFT_BGR
  // This board's panel is wired B-G-R. Arduino_ST7789 hardcodes the MADCTL RGB
  // order, so re-issue MADCTL with the BGR bit (0x08) set on every rotation
  // change. Only rotations 0-3 are used by the SmallTV (setRotation(r & 3)).
  void setRotation(uint8_t r) override {
    Arduino_TFT::setRotation(r);           // updates _rotation + width/height
    uint8_t madctl;
    switch (_rotation) {
      case 1:  madctl = ST7789_MADCTL_MX | ST7789_MADCTL_MV; break;
      case 2:  madctl = ST7789_MADCTL_MX | ST7789_MADCTL_MY; break;
      case 3:  madctl = ST7789_MADCTL_MY | ST7789_MADCTL_MV; break;
      default: madctl = 0; break;          // case 0
    }
    madctl |= 0x08;                         // BGR
    _bus->beginWrite();
    _bus->writeC8D8(ST7789_MADCTL, madctl);
    _bus->endWrite();
  }
#endif
};

static Arduino_DataBus* bus = nullptr;
static Arduino_GFX*     gfx = nullptr;

Arduino_GFX* gfxDev() { return gfx; }

// ---------------------------------------------------------------------------
void gfxBegin(const Settings& s) {
#ifdef TFT_PWR_PIN
  // Boards with a switched panel power rail (NM-TV-154): energize the display
  // before anything else or the panel never comes up.
  pinMode(TFT_PWR_PIN, OUTPUT);
  digitalWrite(TFT_PWR_PIN, TFT_PWR_ON);
#endif
  // Backlight FIRST: do it before the panel/SPI init so the screen lights up even
  // if panel init has trouble. A dark backlight then means the sketch didn't get
  // this far (early crash / bad flash) — a useful boot indicator.
  pinMode(TFT_BL, OUTPUT);
  platformAnalogWriteInit(TFT_BL);
  gfxSetBrightness(s.brightness, s.backlightInverted);

#if defined(SMALLTV_ESP32C2) || defined(SMALLTV_ESP32)
  // Hardware SPI via the Arduino SPI library (IDF spi_master driver) on explicit
  // GPIOs. The register-level Arduino_ESP32SPI hangs in begin() on the C2, and
  // Arduino_SWSPI's fast-IO path doesn't cover the C2 — Arduino_HWSPI uses the
  // stock driver (what the working ESPHome config used) and honors SPI mode 3
  // (see the subclass). Pins come from the board header; a TFT_CS of -1 means
  // the panel's CS is tied to GND and is never toggled.
  bus = new Arduino_HWSPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, GFX_NOT_DEFINED, &SPI);
#else
  bus = new Arduino_HWSPI(TFT_DC, TFT_CS);   // ESP8266 HW-SPI (fixed SCLK/MOSI)
#endif
  // IPS=true so the panel colors are not inverted; full 240x240, no offsets.
  // Use the SmallTV variant so the SPI bus comes up in mode 3 (see class above).
  gfx = new Arduino_ST7789_SmallTV(bus, TFT_RST, 0 /*rotation*/, true /*IPS*/,
                                   TFT_WIDTH, TFT_HEIGHT, 0, 0, 0, 0);
  gfx->begin();
  gfx->setRotation(s.rotation & 3);
  // Nothing in this UI ever wants wrapped text: overflowing labels used to
  // wrap around to x=0 on the next line (stray characters at the left edge).
  gfx->setTextWrap(false);
  gfx->fillScreen(C_BLACK);
}

void gfxSetBrightness(uint8_t pct, bool inverted) {
  if (pct > 100) pct = 100;
  int duty = (int)pct * 255 / 100;
  if (inverted) duty = 255 - duty;
  analogWrite(TFT_BL, duty);
}

void gfxSetRotation(uint8_t r) {
  if (gfx) gfx->setRotation(r & 3);
}

// ---- text helpers (built-in 6x8 font, integer scaled) ---------------------
int gfxTextW(const char* s, uint8_t size) { return (int)strlen(s) * 6 * size; }

// ---- The typeface ---------------------------------------------------------
#include "TextFonts.h"

static bool s_typeSans = false;
void gfxTypeSans(bool on) { s_typeSans = on; }
bool gfxTypeIsSans()      { return s_typeSans; }

// Which sans face stands in for a pixel size. Only 3 and 2 have one — size 1
// is too small for anything but the pixel font, and nothing draws text at 4+.
static int textFaceFor(uint8_t size) {
  if (size == 3) return 0;
  if (size == 2) return 1;
  return -1;
}

static int textFaceW(const char* s, int face) {
  const GFXfont* f = kTextFaces[face].font;
  int w = 0;
  for (const char* p = s; *p; p++) {
    if ((uint8_t)*p < TEXT_FONT_FIRST || (uint8_t)*p > TEXT_FONT_LAST) return -1;
    const GFXglyph* g = &f->glyph[(uint8_t)*p - f->first];
    w += pgm_read_byte(&g->xAdvance);
  }
  return w;
}

// Top-of-band to baseline. The pixel value is 8*size — the constant the sites
// that align a smaller unit against a big number's baseline always used.
int gfxLabelAscent(uint8_t size) {
  if (s_typeSans) {
    const int face = textFaceFor(size);
    if (face >= 0) return kTextFaces[face].ascent;
  }
  return 8 * size;
}

int gfxLabelW(const char* s, uint8_t size) {
  if (s_typeSans) {
    const int face = textFaceFor(size);
    if (face >= 0) {
      const int w = textFaceW(s, face);
      if (w >= 0) return w;
    }
  }
  return gfxTextW(s, size);
}

// Draw a label with `topY` as the top of its pixel band. The sans faces fit
// the band by construction, so this can substitute for the pixel font at any
// call site without re-checking its layout. Falls back to pixel when the
// string has a character the face lacks, or when sans would overflow a line
// the caller had pixel-fit (all-caps sans can run a little wider per char).
void gfxLabel(int x, int topY, const char* s, uint8_t size, uint16_t color) {
  if (!gfx) return;
  const int face = s_typeSans ? textFaceFor(size) : -1;
  const int w = face >= 0 ? textFaceW(s, face) : -1;
  if (w >= 0 && x + w <= TFT_WIDTH) {
    gfx->setFont(kTextFaces[face].font);
    gfx->setTextSize(1);
    gfx->setTextColor(color);
    gfx->setCursor(x, topY + kTextFaces[face].ascent);
    gfx->print(s);
    gfx->setFont(nullptr);
  } else {
    gfx->setTextSize(size);
    gfx->setTextColor(color);
    gfx->setCursor(x, topY);
    gfx->print(s);
  }
}

void gfxDrawCentered(const char* s, int y, uint8_t size, uint16_t color) {
  if (!gfx) return;
  int x = (TFT_WIDTH - gfxLabelW(s, size)) / 2;
  if (x < 0) x = 0;
  gfxLabel(x, y, s, size, color);
}

// ---- The numbers face -----------------------------------------------------
#include "NumFonts.h"
static_assert(NUM_FACES == NUM_FONT_COUNT,
              "config.h and the generated NumFonts.h disagree on the face count");

bool gfxNumEligible(const char* s) {
  if (!s || !s[0]) return false;
  for (const char* p = s; *p; p++)
    if ((uint8_t)*p < NUM_FONT_FIRST || (uint8_t)*p > NUM_FONT_LAST) return false;
  return true;
}

int gfxNumFaceW(const char* s, int face) {
  if (face < 0 || face >= NUM_FONT_COUNT) return 0;
  const GFXfont* f = kNumFaces[face].font;
  int w = 0;
  for (const char* p = s; *p; p++) {
    const GFXglyph* g = &f->glyph[(uint8_t)*p - f->first];
    w += pgm_read_byte(&g->xAdvance);
  }
  return w;
}

int gfxNumFace(const char* s, int maxW) {
  for (int i = 0; i < NUM_FONT_COUNT; i++)
    if (gfxNumFaceW(s, i) <= maxW) return i;
  return NUM_FONT_COUNT - 1;     // nothing fits: the smallest clips least
}

int gfxNumFaceAscent(int face) {
  if (face < 0 || face >= NUM_FONT_COUNT) return 0;
  return kNumFaces[face].ascent;
}

int gfxNumFaceH(int face) {
  if (face < 0 || face >= NUM_FONT_COUNT) return 0;
  return kNumFaces[face].ascent + kNumFaces[face].descent;
}

void gfxNumFaceDraw(int x, int topY, const char* s, int face, uint16_t color) {
  if (!gfx || face < 0 || face >= NUM_FONT_COUNT) return;
  gfx->setFont(kNumFaces[face].font);
  gfx->setTextSize(1);
  gfx->setTextColor(color);
  gfx->setCursor(x, topY + kNumFaces[face].ascent);   // GFXfonts draw from the baseline
  gfx->print(s);
  gfx->setFont(nullptr);       // back to the classic font for whoever draws next
}

// ---- Marquee --------------------------------------------------------------
// The phase arithmetic lives in GfxMarqueeStep.h so it can be checked on a
// host; what is left here is the drawing.
bool gfxMarqueeDraw(const GfxMarquee& m, const char* s, uint32_t phaseMs) {
  if (!gfx || !s) return false;
  const int h = 8 * m.size;
  const int textW = gfxTextW(s, m.size);

  gfx->setTextSize(m.size);
  // Opaque text: every glyph paints its own background, so consecutive frames
  // overwrite each other cleanly. Clearing the whole band first would flicker,
  // there being no framebuffer to compose in.
  gfx->setTextColor(m.fg, m.bg);

  if (textW <= m.w) {                       // fits: centre it and stay put
    gfx->fillRect(m.x, m.y, m.w, h, m.bg);
    gfx->setCursor(m.x + (m.w - textW) / 2, m.y);
    gfx->print(s);
    return false;
  }

  const GfxMarqueeStep step = gfxMarqueeStepAt(textW, m.w, phaseMs);
  const int off = step.off, total = step.total;

  // Clip to the band so a glyph hanging off either end cannot scribble on the
  // cover above or the progress bar below.
  gfx->setTextBound(m.x, m.y, m.w, h);
  gfx->setCursor(m.x - off, m.y);
  gfx->print(s);
  gfx->setCursor(m.x - off + total, m.y);   // the repeat, for a seamless wrap
  gfx->print(s);

  // The gap is the one span with no glyph to paint its own background.
  int gx0 = m.x - off + textW, gx1 = gx0 + MARQUEE_GAP_PX;
  if (gx0 < m.x) gx0 = m.x;
  if (gx1 > m.x + m.w) gx1 = m.x + m.w;
  if (gx1 > gx0) gfx->fillRect(gx0, m.y, gx1 - gx0, h, m.bg);

  gfx->setTextBound(0, 0, TFT_WIDTH, TFT_HEIGHT);
  return true;
}

// Largest size (<= maxSize) whose rendered width fits within maxW.
uint8_t gfxFitSize(const char* s, int maxW, uint8_t maxSize) {
  for (uint8_t sz = maxSize; sz > 1; sz--) {
    if (gfxTextW(s, sz) <= maxW) return sz;
  }
  return 1;
}

// ---------------------------------------------------------------------------
void gfxBoot(const char* line1, const char* line2) {
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  gfxDrawCentered(line1, 95, 3, C_WHITE);
  if (line2 && line2[0]) gfxDrawCentered(line2, 130, 2, C_GRAY);
}

void gfxApInfo(const char* ssid, const char* pass, const char* ip) {
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  gfxDrawCentered("SETUP MODE", 18, 3, C_YELLOW);
  gfxDrawCentered("Join WiFi:", 64, 2, C_GRAY);
  gfxDrawCentered(ssid, 88, gfxFitSize(ssid, 232, 3), C_WHITE);
  if (pass && pass[0]) {
    gfxDrawCentered("Password:", 124, 2, C_GRAY);
    gfxDrawCentered(pass, 146, gfxFitSize(pass, 232, 2), C_WHITE);
  } else {
    gfxDrawCentered("(open network)", 124, 2, C_GRAY);
  }
  gfxDrawCentered("Then open:", 182, 2, C_GRAY);
  String url = String("http://") + ip;
  gfxDrawCentered(url.c_str(), 206, gfxFitSize(url.c_str(), 232, 2), C_GREEN);
}

void gfxStaInfo(const char* ssid, const char* ip, const char* host) {
  // The SSID is the one string on this screen the outside world writes;
  // fold it for display (the joining bytes elsewhere stay exact).
  char sbuf[40];
  strlcpy(sbuf, ssid && ssid[0] ? ssid : "-", sizeof(sbuf));
  textFoldUtf8(sbuf);
  ssid = sbuf;
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  gfxDrawCentered("CONNECTED", 18, 3, C_GREEN);
  gfxDrawCentered("Network:", 62, 2, C_GRAY);
  gfxDrawCentered(ssid && ssid[0] ? ssid : "-", 84, gfxFitSize(ssid, 232, 3), C_WHITE);
  gfxDrawCentered("Open in browser:", 126, 2, C_GRAY);
  // IP shown big (always fits at size 2); mDNS name below as a friendlier option.
  gfxDrawCentered(ip && ip[0] ? ip : "-", 150, gfxFitSize(ip, 232, 3), C_GREEN);
  if (host && host[0]) {
    String url = String("http://") + host + ".local";
    gfxDrawCentered(url.c_str(), 188, gfxFitSize(url.c_str(), 232, 2), C_GRAY);
  }
}

void gfxMessage(const char* title, const char* msg, uint16_t titleColor) {
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  gfxDrawCentered(title, 90, 3, titleColor);
  if (msg && msg[0]) gfxDrawCentered(msg, 130, 2, C_GRAY);
}

// Persistent crash screen shown in safe mode (after an exception reset). Holds the
// crash PC + fault address still so they can be read, and the IP for OTA recovery.
void gfxCrash(const char* epc, const char* addr, const char* ip,
              uint8_t resumeMin) {
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);
  gfxDrawCentered("CRASH", 12, 4, C_RED);
  gfxDrawCentered("epc", 60, 2, C_GRAY);
  gfxDrawCentered(epc && epc[0] ? epc : "-", 80, 3, C_WHITE);
  gfxDrawCentered("addr", 124, 2, C_GRAY);
  gfxDrawCentered(addr && addr[0] ? addr : "-", 146, 2, C_WHITE);
  char rl[24];
  snprintf(rl, sizeof(rl), "resumes in %u min", (unsigned)resumeMin);
  gfxDrawCentered(resumeMin ? rl : "OTA flash to fix:", 182, 2, C_GRAY);
  gfxDrawCentered(ip && ip[0] ? ip : "-", 204, 2, C_GREEN);
}
