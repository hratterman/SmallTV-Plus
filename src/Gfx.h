// Gfx.h — shared ST7789 device, drawing primitives, and boot/status screens.
//
// This is the core display layer. The three feature modes (ticker / usage / radar)
// render on top of it via gfxDev() and the exposed text helpers; each feature owns
// its own feature-specific rendering. Nothing feature-specific lives here.
#pragma once
#include <Arduino.h>
#include "Settings.h"

class Arduino_GFX;   // fwd-decl: only the drawing .cpp files pull in the full lib

// ---- Shared colors (RGB565) ----------------------------------------------
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GREEN  0x07E0
#define C_RED    0xF800
#define C_GRAY   0x8410
#define C_DGRAY  0x4208
#define C_YELLOW 0xFFE0
#define C_BLUE   0x041F

// ---- Device lifecycle -----------------------------------------------------
void         gfxBegin(const Settings& s);
void         gfxSetBrightness(uint8_t pct, bool inverted);
void         gfxSetRotation(uint8_t r);
Arduino_GFX* gfxDev();                 // shared draw target for feature renderers

// ---- Text helpers (built-in 6x8 font, integer scaled) ---------------------
int     gfxTextW(const char* s, uint8_t size);
void    gfxDrawCentered(const char* s, int y, uint8_t size, uint16_t color);
uint8_t gfxFitSize(const char* s, int maxW, uint8_t maxSize);

// ---- Marquee --------------------------------------------------------------
// A band of text that sits still when it fits and scrolls when it doesn't,
// the way a car radio or Spotify handles a title too long for the panel.
// Shrinking the font or cutting the end off with an ellipsis both lose the
// information; scrolling is the only option that keeps all of it.
struct GfxMarquee {
  int16_t  x, y, w;    // the band; height is 8 * size
  uint8_t  size;
  uint16_t fg, bg;
};

// Draws one frame. `phaseMs` is a free-running clock — pass millis() and the
// text advances on its own. Returns true when the text is long enough to be
// scrolling, so the caller knows this band needs to keep being redrawn.
bool gfxMarqueeDraw(const GfxMarquee& m, const char* s, uint32_t phaseMs);

// ---- The numbers face -----------------------------------------------------
// Sites that show a big numeral (ticker price, usage %, miner hashrate) draw
// it through these, so whether it comes out pixel or sans is one setting
// checked in one place. The sans faces carry only 0x20..0x3A — a string with
// anything else in it is not eligible and the caller keeps its pixel path.
// `topY` is the top of the band in both worlds; the helper owns the baseline.
bool gfxNumEligible(const char* s);
int  gfxNumFace(const char* s, int maxW);          // largest face fitting maxW
int  gfxNumFaceW(const char* s, int face);
int  gfxNumFaceH(int face);                        // ascent + descent
int  gfxNumFaceAscent(int face);                   // baseline below the band top
void gfxNumFaceDraw(int x, int topY, const char* s, int face, uint16_t color);

// ---- Shared boot / status / diagnostic screens ----------------------------
void gfxBoot(const char* line1, const char* line2);
void gfxApInfo(const char* ssid, const char* pass, const char* ip);
void gfxStaInfo(const char* ssid, const char* ip, const char* host);
void gfxMessage(const char* title, const char* msg, uint16_t titleColor);
void gfxCrash(const char* epc, const char* addr, const char* ip);  // safe-mode diag
