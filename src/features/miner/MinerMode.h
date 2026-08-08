// MinerMode.h — bitcoin solo-miner feature (the NM-TV's original calling).
//
// Ports the mining core of BitMaker-hub/NerdMiner_v2 into the smalltv-mod mode
// architecture: a stratum task owns the pool connection and prepares jobs, hash
// workers grind nonces on both cores, and this DisplayMode only renders the
// stats they publish. Mining runs whenever it is enabled and a BTC address is
// configured, regardless of which mode is on screen — switching the carousel
// away does not drop the pool connection.
#pragma once
#include "Mode.h"
#include "config.h"

class MinerMode : public DisplayMode {
 public:
  const char* id() const override { return "miner"; }
  uint8_t     modeConst() const override { return MODE_MINER; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override { needFull_ = true; }  // repaint only

 private:
  void render(const Settings& s, bool full);
  void renderChrome(const Settings& s);   // header, panels, row labels

  bool     needFull_ = true;   // repaint static parts (labels, header) too
  uint32_t lastDraw_ = 0;      // 1 s value-refresh cadence
};

extern MinerMode g_minerMode;
