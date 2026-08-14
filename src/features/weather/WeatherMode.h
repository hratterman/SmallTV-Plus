#pragma once
#include "config.h"
#if WITH_WEATHER

#include "../../Mode.h"
#include "WeatherClient.h"

class WeatherMode : public DisplayMode {
 public:
  const char* id() const override { return "weather"; }
  uint8_t     modeConst() const override { return MODE_WEATHER; }
  void begin(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override;
  void service(const Settings& s) override;
  // With a radar animation ready, ask the carousel for the extra seconds one
  // pass of it takes; without one, 0 defers to the normal dwell.
  uint16_t dwellSec(const Settings& s) const override;
  // Long-press: jump straight to the radar loop instead of waiting for it.
  void onContextAction(Settings& s) override;

 private:
  void render(const Settings& s, const WeatherData& w);
  bool drawRadarFrame();               // false: radar went away mid-animation
  bool     needFull_ = true;
  uint32_t renderedOk_ = 0xFFFFFFFF;
  bool     renderedErr_ = false;
  uint32_t lastDrawMs_ = 0;
  bool     footAlt_ = false;   // footer alternates age with the radar note

  // The radar sub-screen: conditions and the timelapse share this mode's slot,
  // alternating while there is an animation worth showing.
  bool     subRadar_   = false;
  uint8_t  rFrame_     = 0;
  uint8_t  rFrames_    = 0;    // count seen at the last draw
  uint8_t  rLoops_     = 0;
  uint32_t rTick_      = 0;
  uint32_t condSince_  = 0;    // when the conditions screen last took over
};

extern WeatherMode g_weatherMode;

#endif  // WITH_WEATHER
