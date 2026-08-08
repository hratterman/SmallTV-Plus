#include "Notify.h"
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include "Gfx.h"

#define NOTIFY_MAX_TEXT 160
#define NOTIFY_MAX_SEC  120

static char     s_text[NOTIFY_MAX_TEXT + 1];
static uint32_t s_until = 0;
static uint16_t s_color = C_WHITE;
static bool     s_drawn = false;

void notifyShow(const char* text, uint16_t sec, uint16_t color) {
  if (!text || !text[0]) return;
  strlcpy(s_text, text, sizeof(s_text));
  if (sec == 0) sec = 8;
  if (sec > NOTIFY_MAX_SEC) sec = NOTIFY_MAX_SEC;
  s_color = color;
  s_until = millis() + (uint32_t)sec * 1000UL;
  s_drawn = false;
}

// "#rrggbb" or "#rgb" -> RGB565. Anything unparseable stays white.
static uint16_t parseColor(const char* h) {
  if (!h || *h != '#') return C_WHITE;
  h++;
  uint32_t v = strtoul(h, nullptr, 16);
  uint8_t r, g, b;
  if (strlen(h) == 3) {
    r = ((v >> 8) & 0xF) * 17; g = ((v >> 4) & 0xF) * 17; b = (v & 0xF) * 17;
  } else {
    r = (v >> 16) & 0xFF; g = (v >> 8) & 0xFF; b = v & 0xFF;
  }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

bool notifyApply(const String& json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  const char* text = doc["text"] | "";
  if (!text[0]) return false;
  notifyShow(text, (uint16_t)(doc["sec"] | 8), parseColor(doc["color"] | "#ffffff"));
  return true;
}

bool notifyActive() { return s_until && (int32_t)(millis() - s_until) < 0; }

void notifyDismiss() { s_until = 0; s_drawn = false; }

// Word-wrapped centred text. The built-in font is 6x8 per size unit, so the
// size is chosen to fit the longest word as well as the line count.
static void drawBanner() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillScreen(C_BLACK);

  // A slim accent bar rather than a full-screen colour wash: the message stays
  // readable whatever colour was pushed.
  gfx->fillRect(0, 0, TFT_WIDTH, 6, s_color);
  gfx->fillRect(0, TFT_HEIGHT - 6, TFT_WIDTH, 6, s_color);

  const int maxW = TFT_WIDTH - 16;
  uint8_t size = 3;
  char lines[6][32];
  int lineCount = 0;

  // Try progressively smaller text until the message fits in six lines.
  for (; size >= 1; size--) {
    const int perLine = maxW / (6 * size);
    if (perLine < 4) continue;
    lineCount = 0;
    const char* p = s_text;
    bool ok = true;
    while (*p && lineCount < 6) {
      while (*p == ' ') p++;
      if (!*p) break;
      int take = 0, lastSpace = -1;
      while (p[take] && take < perLine) {
        if (p[take] == ' ') lastSpace = take;
        take++;
      }
      if (p[take] && lastSpace > 0) take = lastSpace;      // break on a word
      if (take > (int)sizeof(lines[0]) - 1) take = sizeof(lines[0]) - 1;
      memcpy(lines[lineCount], p, take);
      lines[lineCount][take] = 0;
      lineCount++;
      p += take;
    }
    if (!*p) { ok = true; } else { ok = false; }
    if (ok && lineCount) break;
    if (size == 1) break;
  }
  if (!lineCount) return;

  const int lineH = 8 * size + 4;
  int y = (TFT_HEIGHT - lineCount * lineH) / 2;
  for (int i = 0; i < lineCount; i++) {
    gfxDrawCentered(lines[i], y, size, C_WHITE);
    y += lineH;
  }
}

void notifyService() {
  if (!notifyActive()) {
    s_drawn = false;
    return;
  }
  if (!s_drawn) {
    s_drawn = true;
    drawBanner();
  }
}
