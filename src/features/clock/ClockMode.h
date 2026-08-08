// ClockMode.h — the cube as a desk clock.
//
// Time comes from the shared SNTP client in Clock.cpp, which already knows the
// timezone and handles DST from the POSIX rule. This mode only draws it: hours
// and minutes large, seconds and date subordinate, in the same dark-panel
// language as the other screens.
#pragma once
#include "config.h"
#if WITH_CLOCK

#include "Mode.h"

class ClockMode : public DisplayMode {
 public:
  const char* id() const override { return "clock"; }
  uint8_t     modeConst() const override { return MODE_CLOCK; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override { needFull_ = true; }
  void onContextAction(Settings& s) override;   // long-press: 12h/24h

 private:
  void render(const Settings& s, bool full);

  bool     needFull_ = true;
  int      lastMin_ = -1;      // redraw the big digits only when they change
  int      lastSec_ = -1;
  int      lastDay_ = -1;
};

extern ClockMode g_clockMode;

#endif  // WITH_CLOCK
