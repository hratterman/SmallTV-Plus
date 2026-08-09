#include "config.h"
#if WITH_SPOTIFY

#include "AlbumArt.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp32/rom/tjpgd.h>
#include "SpotifyClient.h"

// TJpgDec's scratch pool. 3100 bytes is the documented figure for baseline
// JPEGs; the spare covers 4:4:4 covers, which some labels use.
#define ART_WORK_SZ 4096

namespace {

struct ArtCtx {
  WiFiClient* stream;
  int32_t  remaining;    // bytes the server said are left, -1 if chunked
  int16_t  ox, oy;       // where the top-left of the image lands on screen
  uint32_t deadline;
};

// TJpgDec pulls input through here. buff == nullptr means "skip these bytes",
// which for a socket is a read into nowhere rather than a seek.
UINT artIn(JDEC* jd, BYTE* buff, UINT nbyte) {
  ArtCtx* c = (ArtCtx*)jd->device;
  if (!c->stream) return 0;

  UINT done = 0;
  while (done < nbyte) {
    if ((int32_t)(millis() - c->deadline) >= 0) break;
    if (c->remaining == 0) break;

    const int avail = c->stream->available();
    if (avail <= 0) {
      if (!c->stream->connected()) break;
      delay(2);                       // let lwIP refill; the miner keeps running
      continue;
    }
    int want = (int)(nbyte - done);
    if (avail < want) want = avail;
    if (c->remaining > 0 && c->remaining < want) want = c->remaining;

    int got;
    if (buff) {
      got = c->stream->readBytes(buff + done, want);
    } else {
      uint8_t sink[64];
      got = c->stream->readBytes(sink, want > (int)sizeof(sink) ? (int)sizeof(sink) : want);
    }
    if (got <= 0) break;
    done += got;
    if (c->remaining > 0) c->remaining -= got;
  }
  return done;
}

// One decoded block. The ROM decoder is built for RGB888, so convert here and
// push a row at a time — the panel takes a run of pixels far more cheaply than
// it takes individual ones.
UINT artOut(JDEC* jd, void* bitmap, JRECT* rect) {
  ArtCtx* c = (ArtCtx*)jd->device;
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return 0;

  const uint8_t* src = (const uint8_t*)bitmap;
  const int w = rect->right - rect->left + 1;
  const int h = rect->bottom - rect->top + 1;

  uint16_t row[SPOTIFY_ART_PX];
  for (int yy = 0; yy < h; yy++) {
    const int py = rect->top + yy;
    if (py >= SPOTIFY_ART_PX) break;              // clip anything oversized
    int n = 0;
    for (int xx = 0; xx < w; xx++) {
      const int px = rect->left + xx;
      const uint8_t* p = src + ((size_t)yy * w + xx) * 3;
      if (px >= SPOTIFY_ART_PX) break;
      row[n++] = ((uint16_t)(p[0] & 0xF8) << 8) |
                 ((uint16_t)(p[1] & 0xFC) << 3) |
                 (p[2] >> 3);
    }
    if (n) gfx->draw16bitRGBBitmap(c->ox + rect->left, c->oy + py, row, n, 1);
  }
  return 1;
}

}  // namespace

static char s_status[40] = "not tried";

const char* albumArtStatus() { return s_status; }

bool albumArtDraw(const char* url, int16_t x, int16_t y) {
  if (!url || !url[0]) { strlcpy(s_status, "no art url", sizeof(s_status)); return false; }

  // The poll task can be mid-handshake on core 0 right now, and two TLS
  // sessions at once do not fit in this heap. Wait for it rather than fail.
  if (!spotifyNetLock(9000)) {
    strlcpy(s_status, "busy: poll holds the radio", sizeof(s_status));
    return false;
  }
  struct NetRelease { ~NetRelease() { spotifyNetUnlock(); } } netRelease;

  WiFiClientSecure client;
  client.setInsecure();          // same posture as the rest of the Spotify path
  client.setTimeout(6);
  HTTPClient http;
  http.setTimeout(6000);
  // The CDN can answer with a redirect, and HTTPClient does not follow one
  // unless asked — which looks identical to the image simply not loading.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    strlcpy(s_status, "begin failed (TLS/heap)", sizeof(s_status));
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(s_status, sizeof(s_status), "HTTP %d", code);
    http.end();
    return false;
  }

  ArtCtx ctx;
  ctx.stream    = http.getStreamPtr();
  ctx.remaining = http.getSize();          // -1 when chunked, handled in artIn
  ctx.ox = x;
  ctx.oy = y;
  ctx.deadline = millis() + 8000;

  void* work = malloc(ART_WORK_SZ);
  if (!work) {
    strlcpy(s_status, "no heap for the decoder", sizeof(s_status));
    http.end();
    return false;
  }

  JDEC jd;
  bool ok = false;
  const JRESULT pr = jd_prepare(&jd, artIn, work, ART_WORK_SZ, &ctx);
  if (pr != JDR_OK) {
    snprintf(s_status, sizeof(s_status), "jd_prepare err %d", (int)pr);
  } else {
    const uint8_t scale = albumArtScaleFor((int)jd.width);
    // Centre whatever size that lands on, so a cover a couple of pixels under
    // the box sits in the middle rather than against the top-left corner.
    const int drawn = (int)(jd.width >> scale);
    if (drawn < SPOTIFY_ART_PX) {
      ctx.ox = (int16_t)(x + (SPOTIFY_ART_PX - drawn) / 2);
      ctx.oy = (int16_t)(y + (SPOTIFY_ART_PX - drawn) / 2);
    }
    const JRESULT dr = jd_decomp(&jd, artOut, scale);
    ok = (dr == JDR_OK);
    if (ok) snprintf(s_status, sizeof(s_status), "ok %dx%d /%d",
                     (int)jd.width, (int)jd.height, 1 << scale);
    else    snprintf(s_status, sizeof(s_status), "jd_decomp err %d", (int)dr);
  }

  free(work);
  http.end();
  return ok;
}

#endif  // WITH_SPOTIFY
