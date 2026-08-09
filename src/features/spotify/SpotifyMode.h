// SpotifyMode.h — what's playing, on the cube.
//
// Track, artist and a progress bar that keeps moving between polls. When
// "take over while playing" is on, main.cpp gives this mode the screen for as
// long as something is actually playing and hands it back when the music stops.
//
// Redraws are split: the panel is painted only when the content actually
// changes (new track, play/pause, an error), while the once-a-second progress
// update touches just the bar's new segment and the elapsed-time label. Clearing
// the whole screen every second is visible as a blink on this panel.
#pragma once
#include "config.h"
#if WITH_SPOTIFY

#include "Mode.h"
#include "SpotifyClient.h"
#include "AlbumArt.h"

class SpotifyMode : public DisplayMode {
 public:
  const char* id() const override { return "spotify"; }
  uint8_t     modeConst() const override { return MODE_SPOTIFY; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  // Another mode has drawn over the screen, so the cover is gone with it.
  void wake(const Settings& s) override { needFull_ = true; artOnGlass_ = false; }
  void onContextAction(Settings& s) override;   // long-press: poll now

 private:
  void renderAll(const Settings& s);       // full panel
  void renderProgress(const Settings& s);  // bar segment + elapsed label only

  bool     needFull_ = true;
  uint32_t lastTick_ = 0;
  uint32_t scrollTick_ = 0;
  bool     scrolling_ = false;   // a band is long enough to need ticking

  // What is currently on the glass, so a repaint only happens on a real change.
  char     drawnTrack_[SPOTIFY_TRACK_LEN]   = {0};
  char     drawnArtist_[SPOTIFY_ARTIST_LEN] = {0};
  bool     drawnPlaying_ = false;
  bool     drawnValid_   = false;
  bool     drawnError_   = false;
  bool     drawnLinked_  = false;
  bool     drawnExplicit_ = false;
  // The cover currently on the glass, and whether it is still intact. Any other
  // mode drawing invalidates it, which is exactly what wake() means.
  char     drawnArt_[SPOTIFY_ART_LEN] = {0};
  bool     artOnGlass_ = false;
  bool     artFailed_  = false;   // last cover attempt did not land
  int      barW_    = -1;   // filled pixels currently drawn
  int      elapsed_ = -1;   // seconds currently shown
};

extern SpotifyMode g_spotifyMode;

#endif  // WITH_SPOTIFY
