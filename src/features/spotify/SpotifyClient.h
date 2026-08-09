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
// Spotify's art URLs are a fixed shape: https://i.scdn.co/image/<40 hex>.
#define SPOTIFY_ART_LEN    72

struct SpotifyData {
  bool     valid;        // a successful poll has happened
  bool     playing;      // something is playing right now
  char     track[SPOTIFY_TRACK_LEN];
  char     artist[SPOTIFY_ARTIST_LEN];
  uint32_t progressMs;
  uint32_t durationMs;
  uint32_t lastOkMs;     // millis() of the last good poll
  uint32_t startedAtMs;  // millis() when progressMs was sampled, for interpolation
  char     artUrl[SPOTIFY_ART_LEN];   // album cover, "" if the track has none
  uint16_t artPx;        // the cover's native square size, for picking a scale
  // Spotify's own flag for the track. Far better than guessing from the title:
  // it is the label's marking, it covers lyrics the title says nothing about,
  // and it needs no word list to maintain.
  bool     explicitTrack;
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

// Cover entries the last poll saw in the payload, before any were rejected for
// size or length. Reported so an empty artUrl can be told apart from a cover
// that failed to download.
uint8_t spotifyArtCandidates();

// One TLS session at a time. The poll runs on its own task and the album art
// fetch runs on the render task; concurrent handshakes do not fit in the heap.
bool spotifyNetLock(uint32_t waitMs);
void spotifyNetUnlock();

#endif  // WITH_SPOTIFY
