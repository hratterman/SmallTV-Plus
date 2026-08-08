// MinerMode.cpp — miner screen. M1 skeleton: placeholder screen proving the
// mode registration, settings slice, and web UI tab; no networking yet.
#include "config.h"
#if WITH_MINER

#include "MinerMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"

MinerMode g_minerMode;

void MinerMode::begin(const Settings& s) {
}

void MinerMode::invalidate(const Settings& s) {
  needFull_ = true;
}

// Shorten a bech32/base58 address for one screen line: head...tail.
static void shortAddr(const String& a, char* out, size_t outLen) {
  if (a.length() <= 20) { strlcpy(out, a.c_str(), outLen); return; }
  snprintf(out, outLen, "%.9s...%s", a.c_str(), a.c_str() + a.length() - 8);
}

void MinerMode::render(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  gfx->fillScreen(C_BLACK);
  gfxDrawCentered("MINER", 24, 3, C_YELLOW);

  char line[48];
  snprintf(line, sizeof(line), "%s:%u", s.miner.poolHost.c_str(),
           (unsigned)s.miner.poolPort);
  gfxDrawCentered(line, 60, 1, C_GRAY);

  if (!s.miner.btcAddress.length()) {
    gfxDrawCentered("no BTC address", 110, 2, C_WHITE);
    gfxDrawCentered("set one in the web UI", 134, 1, C_GRAY);
  } else {
    char addr[24];
    shortAddr(s.miner.btcAddress, addr, sizeof(addr));
    gfxDrawCentered(addr, 110, 1, C_WHITE);
    gfxDrawCentered(s.miner.enabled ? "mining core: not built yet"
                                    : "mining disabled", 134, 1, C_GRAY);
  }
}

void MinerMode::service(const Settings& s) {
  if (!needFull_) return;   // static placeholder: draw once until invalidated
  needFull_ = false;
  render(s);
}

#endif  // WITH_MINER
