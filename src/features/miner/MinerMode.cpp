// MinerMode.cpp — the miner screen. The mining itself lives in MinerCore and
// runs regardless of which mode is displayed; this only renders its stats.
//
// Layout follows the usage meter's visual language (rounded dark panels, one
// big number, dim labels). Redraws are per field: the chrome is painted once and
// each value repaints only its own slot when it actually changes, so a 1 Hz
// refresh never flickers.
#include "config.h"
#if WITH_MINER

#include "MinerMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "MinerCore.h"

MinerMode g_minerMode;

#define C_BTC    0xF483   // bitcoin orange #f7931a
#define C_PANEL  0x18E3   // card fill, same as the usage meter
#define C_DIM    0xB574   // secondary text

// Panel geometry
static const int PANEL_X = 8, PANEL_W = 224;
static const int RATE_Y = 34, RATE_H = 68;
static const int STAT_Y = 110, STAT_H = 92;
static const int ROW0_Y = 120, ROW_DY = 22;
static const int VAL_R = 220, VAL_SLOT = 132;   // right edge + erase width

// Last-drawn text per slot, so a field only repaints when it changes. The
// footer gets its own size: it carries the pool's error text, which is longer
// than any number on this screen, and a slot too small to hold it would also
// make two different errors compare equal and skip the repaint.
struct Slot { char text[24]; };
struct FootSlot { char text[64]; };
static Slot s_state, s_rate, s_rateUnit, s_rows[4];
static FootSlot s_foot;

static void slotClear() {
  s_state.text[0] = s_rate.text[0] = s_rateUnit.text[0] = s_foot.text[0] = 0;
  for (auto& r : s_rows) r.text[0] = 0;
}

// ---- formatting -----------------------------------------------------------
static void fmtRate(uint32_t hs, char* num, size_t numLen, char* unit, size_t unitLen) {
  if (hs >= 1000000UL) {
    snprintf(num, numLen, "%.2f", hs / 1000000.0);
    strlcpy(unit, "MH/s", unitLen);
  } else if (hs >= 1000UL) {
    snprintf(num, numLen, "%.1f", hs / 1000.0);
    strlcpy(unit, "KH/s", unitLen);
  } else {
    snprintf(num, numLen, "%lu", (unsigned long)hs);
    strlcpy(unit, "H/s", unitLen);
  }
}

// Difficulty spans many orders of magnitude; give it a K/M/G/T suffix.
static void fmtDiff(double d, char* out, size_t n) {
  if (d <= 0)         strlcpy(out, "-", n);
  else if (d >= 1e12) snprintf(out, n, "%.2fT", d / 1e12);
  else if (d >= 1e9)  snprintf(out, n, "%.2fG", d / 1e9);
  else if (d >= 1e6)  snprintf(out, n, "%.2fM", d / 1e6);
  else if (d >= 1e3)  snprintf(out, n, "%.2fK", d / 1e3);
  else if (d >= 1)    snprintf(out, n, "%.2f", d);
  else                snprintf(out, n, "%.4f", d);
}

static void fmtUptime(uint32_t sec, char* out, size_t n) {
  uint32_t d = sec / 86400, h = (sec % 86400) / 3600, m = (sec % 3600) / 60;
  if (d)      snprintf(out, n, "%lud %luh", (unsigned long)d, (unsigned long)h);
  else if (h) snprintf(out, n, "%luh %02lum", (unsigned long)h, (unsigned long)m);
  else        snprintf(out, n, "%lum", (unsigned long)m);
}

static const char* stateLabel(const MinerStats& st) {
  switch (st.state) {
    case MINER_CONNECTING:  return "connecting";
    case MINER_SUBSCRIBED:  return "waiting job";
    case MINER_MINING:      return "mining";
    case MINER_AUTH_FAILED: return "rejected";
    case MINER_NO_SOCKET:   return "no socket";
    default:                return "idle";
  }
}

static uint16_t stateColor(const MinerStats& st) {
  switch (st.state) {
    case MINER_MINING:      return C_GREEN;
    case MINER_SUBSCRIBED:
    case MINER_CONNECTING:  return C_YELLOW;
    case MINER_AUTH_FAILED:
    case MINER_NO_SOCKET:   return C_RED;
    default:                return C_GRAY;
  }
}

// ---- field painting -------------------------------------------------------

// Right-aligned value that erases only its own slot before drawing.
static void drawSlotRight(Arduino_GFX* gfx, Slot& slot, const char* s, int rightX,
                          int y, uint8_t size, uint16_t color, uint16_t bg) {
  if (!strcmp(slot.text, s)) return;
  strlcpy(slot.text, s, sizeof(slot.text));
  gfx->fillRect(rightX - VAL_SLOT, y, VAL_SLOT, 8 * size, bg);
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(rightX - gfxTextW(s, size), y);
  gfx->print(s);
}

void MinerMode::begin(const Settings& s) {
  minerCoreBegin(s);
}

void MinerMode::invalidate(const Settings& s) {
  minerCoreApplyConfig(s);
  needFull_ = true;
}

// Static chrome: header, panels, row labels. Painted only on a full repaint.
void MinerMode::renderChrome(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  gfx->fillScreen(C_BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(C_BTC);
  gfx->setCursor(10, 10);
  gfx->print("MINER");

  gfx->fillRoundRect(PANEL_X, RATE_Y, PANEL_W, RATE_H, 8, C_PANEL);
  gfx->fillRoundRect(PANEL_X, STAT_Y, PANEL_W, STAT_H, 8, C_PANEL);

  static const char* kLabels[4] = {"shares", "best", "pool diff", "jobs"};
  gfx->setTextSize(1);
  gfx->setTextColor(C_DIM);
  for (int i = 0; i < 4; i++) {
    gfx->setCursor(20, ROW0_Y + i * ROW_DY + 4);
    gfx->print(kLabels[i]);
  }
  slotClear();
}

void MinerMode::render(const Settings& s, bool full) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  MinerStats st;
  minerCoreSnapshot(st);

  // Not set up yet: a plain prompt instead of a screen full of zeroes.
  if (!st.configured) {
    if (full) {
      gfx->fillScreen(C_BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(C_BTC);
      gfx->setCursor(10, 10);
      gfx->print("MINER");
      const bool work = st.blockedByWork;
      gfxDrawCentered(work ? "off for work mode"
                           : (s.miner.enabled ? "no BTC address" : "mining disabled"),
                      104, 2, work ? C_DIM : C_WHITE);
      gfxDrawCentered(work ? "not mining on an office network"
                           : "configure in the web UI", 134, 1, C_DIM);
      slotClear();
    }
    return;
  }

  if (full) renderChrome(s);

  // State pill, top right.
  drawSlotRight(gfx, s_state, stateLabel(st), 230, 14, 1, stateColor(st), C_BLACK);

  // Hashrate: big number plus a smaller unit, centred as a group.
  char num[16], unit[8];
  fmtRate(st.hashrate, num, sizeof(num), unit, sizeof(unit));
  if (strcmp(s_rate.text, num) || strcmp(s_rateUnit.text, unit)) {
    strlcpy(s_rate.text, num, sizeof(s_rate.text));
    strlcpy(s_rateUnit.text, unit, sizeof(s_rateUnit.text));
    gfx->fillRect(PANEL_X + 4, RATE_Y + 6, PANEL_W - 8, RATE_H - 12, C_PANEL);
    const uint16_t numC = st.hashrate ? C_WHITE : C_DIM;
    if (s.numFont == NUM_FONT_SANS && gfxNumEligible(num)) {
      // The unit ("kH/s") has letters and stays pixel; only the number changes
      // face. Their baselines align so the pair still reads as one figure.
      int face = gfxNumFace(num, 150);
      while (face < NUM_FACES - 1 && gfxNumFaceH(face) > RATE_H - 12) face++;
      const int nw = gfxNumFaceW(num, face), uw = gfxTextW(unit, 2);
      const int x0 = (TFT_WIDTH - (nw + 8 + uw)) / 2;
      const int ny = RATE_Y + (RATE_H - gfxNumFaceH(face)) / 2;
      gfxNumFaceDraw(x0, ny, num, face, numC);
      gfx->setTextSize(2);
      gfx->setTextColor(C_DIM);
      gfx->setCursor(x0 + nw + 8, ny + gfxNumFaceAscent(face) - 16);
      gfx->print(unit);
    } else {
      uint8_t nsz = gfxFitSize(num, 150, 5);
      int nw = gfxTextW(num, nsz), uw = gfxTextW(unit, 2);
      int x0 = (TFT_WIDTH - (nw + 8 + uw)) / 2;
      int ny = RATE_Y + (RATE_H - 8 * nsz) / 2;
      gfx->setTextSize(nsz);
      gfx->setTextColor(numC);
      gfx->setCursor(x0, ny);
      gfx->print(num);
      gfx->setTextSize(2);
      gfx->setTextColor(C_DIM);
      gfx->setCursor(x0 + nw + 8, ny + 8 * nsz - 16);
      gfx->print(unit);
    }
  }

  // Stat rows.
  char buf[24];
  // accepted/submitted, and the reject count once there is one — "0/28" alone
  // cannot distinguish "the pool turned them all down" from "no reply yet".
  if (st.rejected)
    snprintf(buf, sizeof(buf), "%lu/%lu -%lu", (unsigned long)st.accepted,
             (unsigned long)st.shares, (unsigned long)st.rejected);
  else
    snprintf(buf, sizeof(buf), "%lu/%lu", (unsigned long)st.accepted,
             (unsigned long)st.shares);
  drawSlotRight(gfx, s_rows[0], buf, VAL_R, ROW0_Y, 2,
                st.rejected ? C_RED : C_WHITE, C_PANEL);

  fmtDiff(st.bestDiff, buf, sizeof(buf));
  drawSlotRight(gfx, s_rows[1], buf, VAL_R, ROW0_Y + ROW_DY, 2, C_BTC, C_PANEL);

  fmtDiff(st.poolDiff, buf, sizeof(buf));
  drawSlotRight(gfx, s_rows[2], buf, VAL_R, ROW0_Y + 2 * ROW_DY, 2, C_DIM, C_PANEL);

  snprintf(buf, sizeof(buf), "%lu", (unsigned long)st.templates);
  drawSlotRight(gfx, s_rows[3], buf, VAL_R, ROW0_Y + 3 * ROW_DY, 2, C_WHITE, C_PANEL);

  // Footer: normally the pool and how long this run has been going, but a
  // pool complaint displaces it. Shares that all bounce look the same on screen
  // whatever the cause; the pool's own wording is what tells them apart, so it
  // gets the line rather than sitting on a serial port nobody is watching.
  char up[16], foot[64];   // the panel clips; the buffer should not also cut
  const bool bad = st.lastError[0] != 0;
  if (st.state == MINER_NO_SOCKET) {
    // Not the pool's doing, so do not put the pool's name on it.
    snprintf(foot, sizeof(foot), "%s", st.lastError);
  } else if (bad) {
    snprintf(foot, sizeof(foot), "pool: %s", st.lastError);
  } else {
    fmtUptime(st.uptimeSec, up, sizeof(up));
    snprintf(foot, sizeof(foot), "%.40s  %s", st.poolHost, up);
  }
  if (strcmp(s_foot.text, foot)) {
    strlcpy(s_foot.text, foot, sizeof(s_foot.text));
    gfx->fillRect(0, 214, TFT_WIDTH, 10, C_BLACK);
    gfxDrawCentered(foot, 214, 1, bad ? C_RED : C_DIM);
  }
}

void MinerMode::service(const Settings& s) {
  uint32_t now = millis();
  if (!needFull_ && now - lastDraw_ < 1000) return;   // 1 Hz value refresh
  bool full = needFull_;
  needFull_ = false;
  lastDraw_ = now;
  render(s, full);
}

#endif  // WITH_MINER
