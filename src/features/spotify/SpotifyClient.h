// SpotifyClient.h — what's playing on your account, fetched by the cube itself.
//
// Spotify's API is OAuth2. The device holds a long-lived refresh token and
// trades it for a short-lived access token whenever the old one expires, which
// is the only part of the flow that can run unattended. Getting that refresh
// token the first time needs a browser and a loopback redirect URI, which an
// embedded device cannot host — so tools/spotify_auth.py does it once on your
// machine and prints a token to paste into the web UI.
#pragma once
#include "config.h"
#if WITH_SPOTIFY

#include <Arduino.h>
#include "Settings.h"

#define SPOTIFY_TRACK_LEN  64
#define SPOTIFY_ARTIST_LEN 64

struct SpotifyData {
  bool     valid;        // a successful poll has happened
  bool     playing;      // something is playing right now
  char     track[SPOTIFY_TRACK_LEN];
  char     artist[SPOTIFY_ARTIST_LEN];
  uint32_t progressMs;
  uint32_t durationMs;
  uint32_t lastOkMs;     // millis() of the last good poll
  uint32_t startedAtMs;  // millis() when progressMs was sampled, for interpolation
  bool     error;
  char     errorMsg[48];
};

void spotifyInit(const Settings& s);
void spotifyService(const Settings& s);       // polls on its own cadence
const SpotifyData& spotifyGet();
void spotifyForceRefresh();

// True when the account is actively playing and the reading is fresh — used to
// let the mode pull focus while music is on.
bool spotifyIsPlaying(const Settings& s);

// Progress interpolated between polls, so the bar moves smoothly at 1 Hz
// instead of jumping once per poll interval.
uint32_t spotifyProgressNow();

#endif  // WITH_SPOTIFY
