// MinerMode.cpp — the miner screen. The mining itself lives in MinerCore and
// runs regardless of which mode is displayed; this only renders its stats.
#include "config.h"
#if WITH_MINER

#include "MinerMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "MinerCore.h"

MinerMode g_minerMode;

void MinerMode::begin(const Settings& s) {
  minerCoreBegin(s);
}

void MinerMode::invalidate(const Settings& s) {
  minerCoreApplyConfig(s);
  needFull_ = true;
}

// Shorten a bech32/base58 address for one screen line: head...tail.
static void shortAddr(const String& a, char* out, size_t outLen) {
  if (a.length() <= 20) { strlcpy(out, a.c_str(), outLen); return; }
  snprintf(out, outLen, "%.9s...%s", a.c_str(), a.c_str() + a.length() - 8);
}

static const char* stateLabel(MinerPoolState st) {
  switch (st) {
    case MINER_CONNECTING: return "connecting";
    case MINER_SUBSCRIBED: return "waiting for job";
    case MINER_MINING:     return "mining";
    default:               return "idle";
  }
}

static uint16_t stateColor(MinerPoolState st) {
  switch (st) {
    case MINER_MINING:     return C_GREEN;
    case MINER_SUBSCRIBED: return C_YELLOW;
    case MINER_CONNECTING: return C_YELLOW;
    default:               return C_GRAY;
  }
}

void MinerMode::render(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  MinerStats st;
  minerCoreSnapshot(st);

  gfx->fillScreen(C_BLACK);
  gfxDrawCentered("MINER", 18, 3, C_YELLOW);

  if (!st.configured) {
    gfxDrawCentered("no BTC address", 100, 2, C_WHITE);
    gfxDrawCentered("set one in the web UI", 128, 1, C_GRAY);
    return;
  }

  char line[64];
  snprintf(line, sizeof(line), "%s:%u", s.miner.poolHost.c_str(),
           (unsigned)s.miner.poolPort);
  gfxDrawCentered(line, 56, 1, C_GRAY);

  gfxDrawCentered(stateLabel(st.state), 82, 2, stateColor(st.state));

  snprintf(line, sizeof(line), "jobs %lu", (unsigned long)st.templates);
  gfxDrawCentered(line, 116, 2, C_WHITE);

  if (st.poolDiff > 0) {
    snprintf(line, sizeof(line), "pool diff %.5f", st.poolDiff);
    gfxDrawCentered(line, 146, 1, C_GRAY);
  }

  char addr[24];
  shortAddr(s.miner.btcAddress, addr, sizeof(addr));
  gfxDrawCentered(addr, 200, 1, C_DGRAY);
}

void MinerMode::service(const Settings& s) {
  uint32_t now = millis();
  if (!needFull_ && now - lastDraw_ < 1000) return;   // 1 Hz value refresh
  needFull_ = false;
  lastDraw_ = now;
  render(s);
}

#endif  // WITH_MINER
