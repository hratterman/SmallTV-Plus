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

 private:
  void render(const Settings& s, const WeatherData& w);
  bool     needFull_ = true;
  uint32_t renderedOk_ = 0xFFFFFFFF;
  bool     renderedErr_ = false;
  uint32_t lastDrawMs_ = 0;
};

extern WeatherMode g_weatherMode;

#endif  // WITH_WEATHER
