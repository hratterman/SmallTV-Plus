// SpotifyMode.h — what's playing, on the cube.
//
// Track, artist and a progress bar that keeps moving between polls. When
// "take over while playing" is on, main.cpp gives this mode the screen for as
// long as something is actually playing and hands it back when the music stops.
#pragma once
#include "Mode.h"
#include "config.h"

class SpotifyMode : public DisplayMode {
 public:
  const char* id() const override { return "spotify"; }
  uint8_t     modeConst() const override { return MODE_SPOTIFY; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override { needFull_ = true; }
  void onContextAction(Settings& s) override;   // long-press: poll now

 private:
  void render(const Settings& s);

  bool     needFull_ = true;
  uint32_t seenOk_ = 0;      // lastOkMs of the reading already drawn
  uint32_t lastDraw_ = 0;
};

extern SpotifyMode g_spotifyMode;
