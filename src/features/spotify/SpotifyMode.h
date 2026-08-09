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
  void wake(const Settings& s) override { needFull_ = true; }
  void onContextAction(Settings& s) override;   // long-press: poll now

 private:
  void renderAll(const Settings& s);       // full panel
  void renderProgress(const Settings& s);  // bar segment + elapsed label only

  bool     needFull_ = true;
  uint32_t lastTick_ = 0;

  // What is currently on the glass, so a repaint only happens on a real change.
  char     drawnTrack_[SPOTIFY_TRACK_LEN]   = {0};
  char     drawnArtist_[SPOTIFY_ARTIST_LEN] = {0};
  bool     drawnPlaying_ = false;
  bool     drawnValid_   = false;
  bool     drawnError_   = false;
  bool     drawnLinked_  = false;
  // The cover currently on the glass. Art is only re-fetched when the URL
  // changes, so a paused track or a repaint costs no network at all.
  char     drawnArt_[SPOTIFY_ART_LEN] = {0};
  bool     artFailed_ = false;
  int      barW_    = -1;   // filled pixels currently drawn
  int      elapsed_ = -1;   // seconds currently shown
};

extern SpotifyMode g_spotifyMode;

#endif  // WITH_SPOTIFY
