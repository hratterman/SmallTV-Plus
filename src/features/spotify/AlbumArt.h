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

#endif  // WITH_SPOTIFY
