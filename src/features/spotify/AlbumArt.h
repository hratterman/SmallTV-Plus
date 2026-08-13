// AlbumArt.h — fetch a cover and paint it, without ever holding the file.
//
// The ESP32 carries TJpgDec in ROM, so the decoder itself costs no flash. It
// pulls bytes through a callback rather than reading a buffer, which means the
// JPEG can be decoded straight off the TLS socket as it arrives: no staging
// allocation, and the picture fills in top to bottom while it downloads. On a
// device where the miner, the web server and mbedTLS are already sharing ~150 KB
// of heap, not allocating 30 KB is worth more than the neatness of having the
// whole file first.
//
// The visible cost is that a stalled download leaves a half-drawn cover. That
// is a better failure than a blank panel and a timeout, and the next poll
// repaints it anyway.
#pragma once
#include "config.h"
#if WITH_SPOTIFY

#include <Arduino.h>

// Square the cover is drawn at. 300/2 and 640/4 both land near here, which is
// why SpotifyClient prefers those two sources.
#define SPOTIFY_ART_PX 152

// How well a source of `width` pixels can be made to fit the box, and at which
// of TJpgDec's 1/1..1/8 descales. Both callers need this and they must agree:
// SpotifyClient picks which of Spotify's three sizes to download, AlbumArt
// unpacks what arrives, and if the two rules differ the client optimises for a
// scale the decoder will not apply. They did differ — the 300 px source was
// chosen for its 1/2 descale to 150, then decoded at 1/1 and clipped, so the
// screen showed the top-left corner of the cover. One function, both callers.
struct AlbumArtFit {
  uint8_t  scale;   // 0..3, the shift TJpgDec should apply
  uint16_t px;      // resulting edge length
  int      err;     // distance from the box; lower is a better source
};

static inline AlbumArtFit albumArtFit(int width) {
  AlbumArtFit best = {0, 0, 1 << 30};
  for (uint8_t s = 0; s <= 3; s++) {
    const int got = width >> s;
    const int err = got > SPOTIFY_ART_PX ? (got - SPOTIFY_ART_PX) * 2   // cropping
                                         : (SPOTIFY_ART_PX - got);      // margin
    if (err < best.err) { best.scale = s; best.px = (uint16_t)got; best.err = err; }
  }
  return best;
}

// Downloads `url` and draws it with its top-left at (x, y), clipped to a
// SPOTIFY_ART_PX square. Blocking, typically under a second on a good link.
// Returns false if the fetch or the decode failed, having drawn whatever
// arrived; the caller decides what to show instead.
bool albumArtDraw(const char* url, int16_t x, int16_t y);

// Why the last attempt ended the way it did — "ok", an HTTP status, or the
// decoder's complaint. Served from /api/status, because "the art doesn't work"
// is not something anyone can act on and this device has taught that lesson
// more than once already.
const char* albumArtStatus();

// True when the backoff after the last failure has expired, i.e. a call to
// albumArtDraw would genuinely try rather than return false on the spot.
bool albumArtRetryDue();

#endif  // WITH_SPOTIFY
