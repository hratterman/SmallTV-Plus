#include "config.h"
#if WITH_SPOTIFY

#include "AlbumArt.h"
#include <LittleFS.h>
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp32/rom/tjpgd.h>
#include "SpotifyClient.h"
#include "NetFetch.h"
#include "Platform.h"          // platformMaxFreeBlock()
#if WITH_MINER
#include "MinerCore.h"
#endif

// TJpgDec's scratch pool. 3100 bytes is the documented figure for baseline
// JPEGs; the spare covers 4:4:4 covers, which some labels use.
#define ART_WORK_SZ 4096

namespace {

// Why the input stopped. "input stream ended" is what the decoder reports for
// all of these, and they want completely different fixes — a server that hung
// up early, a transfer that stalled, and a Content-Length that was shorter than
// the JPEG are three different problems wearing one message.
enum ArtStop : uint8_t {
  ART_STOP_NONE = 0,
  ART_STOP_STALL,      // nothing arrived for ART_STALL_MS
  ART_STOP_CLOSED,     // the server hung up
  ART_STOP_LENGTH,     // read exactly as many bytes as were promised
};

struct ArtCtx {
  // Three ways in, one way out. Over WiFi the decoder pulls straight off the
  // socket and the picture fills in as it downloads. Over the tether the bytes
  // arrive pushed, a frame at a time, and a pull-driven decoder cannot be fed
  // that way — so they are collected first (RAM when a cover-sized block
  // exists, a flash file when it does not) and the decoder reads that.
  // Same callback, same decode, one branch at the top of it.
  WiFiClient* stream;    // null when reading from memory or a file
  File* file;            // null unless staging through flash
  const uint8_t* mem;
  uint32_t memLen, memPos;
  int32_t  remaining;    // bytes the server said are left, -1 if unknown
  int16_t  ox, oy;       // where the top-left of the image lands on screen
  // A stall clock, not a total-transfer budget: it is pushed forward every time
  // bytes actually arrive. A big cover on a slow hotspot is not an error, and
  // capping the whole download meant a slow one failed the same way a dead
  // connection did.
  uint32_t deadline;
  uint32_t got;          // bytes delivered so far, for the diagnostics
  uint32_t declared;     // Content-Length, 0 when the server did not say
  ArtStop  stop;
  // First bytes off the wire. A decoder error says the file was not acceptable
  // but not whether it was even a JPEG, and those want completely different
  // fixes — ff d8 is a JPEG the ROM decoder will not take (progressive, most
  // likely), 89 50 is a PNG, 52 49 is a WebP the CDN substituted.
  uint8_t  magic[2];
  uint8_t  magicLen;
};

// How long the socket may deliver nothing before we call it dead.
#define ART_STALL_MS 6000

// The ROM decoder returns a bare number. These are TJpgDec's JRESULT values,
// as words, because "err 8" is not something anyone can act on.
const char* artJdErr(int r) {
  switch (r) {
    case 1:  return "interrupted";
    case 2:  return "input stream ended";
    case 3:  return "decoder pool too small";
    case 4:  return "input buffer too small";
    case 5:  return "bad parameter";
    case 6:  return "damaged data";
    case 7:  return "unsupported format";
    case 8:  return "not baseline JPEG";
    default: return "unknown";
  }
}

// TJpgDec pulls input through here. buff == nullptr means "skip these bytes",
// which for a socket is a read into nowhere rather than a seek.
UINT artIn(JDEC* jd, BYTE* buff, UINT nbyte) {
  ArtCtx* c = (ArtCtx*)jd->device;

  if (c->file) {                        // staged on flash: the big-cover path
    UINT got;
    if (buff) {
      got = (UINT)c->file->read(buff, nbyte);
    } else {
      c->file->seek(c->file->position() + nbyte);
      got = nbyte;
    }
    if (buff)
      for (UINT k = 0; k < got && c->magicLen < 2; k++)
        c->magic[c->magicLen++] = buff[k];
    c->got += got;
    if (got < nbyte) c->stop = ART_STOP_LENGTH;
    return got;
  }

  if (!c->stream) {                     // buffered in RAM: the tether path
    const uint32_t left = c->memLen - c->memPos;
    const UINT take = nbyte < left ? nbyte : (UINT)left;
    if (buff && take) memcpy(buff, c->mem + c->memPos, take);
    c->memPos += take;                  // buff == nullptr means skip, same cost
    c->got = c->memPos;
    if (take < nbyte) c->stop = ART_STOP_LENGTH;   // the collected file was short
    return take;
  }

  UINT done = 0;
  while (done < nbyte) {
    if (c->remaining == 0) { c->stop = ART_STOP_LENGTH; break; }
    if ((int32_t)(millis() - c->deadline) >= 0) { c->stop = ART_STOP_STALL; break; }

    const int avail = c->stream->available();
    if (avail <= 0) {
      // connected() alone is not enough: lwIP can report the socket closed
      // while the last records are still buffered, and dropping them truncates
      // an otherwise complete cover.
      if (!c->stream->connected() && c->stream->available() <= 0) {
        c->stop = ART_STOP_CLOSED;
        break;
      }
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
    // The freshly read bytes are buff[done .. done+got); each call appends to
    // the stream, so taking them in order gives the file's first bytes wherever
    // the call boundaries happen to fall.
    if (buff)
      for (int k = 0; k < got && c->magicLen < 2; k++)
        c->magic[c->magicLen++] = buff[done + k];
    done += got;
    c->got += (uint32_t)got;
    c->deadline = millis() + ART_STALL_MS;   // progress: restart the stall clock
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
static uint32_t s_retryAt = 0;     // millis() before which a retry is pointless

const char* albumArtStatus() { return s_status; }

// Whether a fresh call would actually go to the network, or bounce straight
// off the backoff. The mode retries on a timer; without this it could not tell
// a real attempt from a no-op, and would count the no-ops as tries.
bool albumArtRetryDue() {
  return !s_retryAt || (int32_t)(millis() - s_retryAt) >= 0;
}

// A TLS session costs tens of kilobytes and this device is also running the
// miner, the web server and the Spotify poll. Starting a handshake that cannot
// complete does not just fail — it takes the heap the *poll* needed with it,
// which is how one failed cover turned into "player: bad JSON" and stayed
// there. Refuse below the threshold instead of trying anyway.
#define ART_MIN_HEAP 60000

// Shared by both routes: hand a prepared context to the decoder and paint.
static bool artDecode(ArtCtx& ctx, int16_t x, int16_t y) {
  void* work = malloc(ART_WORK_SZ);
  if (!work) {
    strlcpy(s_status, "no heap for the decoder", sizeof(s_status));
    return false;
  }
  JDEC jd;
  bool ok = false;
  const JRESULT pr = jd_prepare(&jd, artIn, work, ART_WORK_SZ, &ctx);
  if (pr != JDR_OK) {
    snprintf(s_status, sizeof(s_status), "%s (%02x%02x)", artJdErr((int)pr),
             ctx.magic[0], ctx.magic[1]);
  } else {
    const AlbumArtFit fit = albumArtFit((int)jd.width);
    if (fit.px < SPOTIFY_ART_PX) {
      ctx.ox = (int16_t)(x + (SPOTIFY_ART_PX - fit.px) / 2);
      ctx.oy = (int16_t)(y + (SPOTIFY_ART_PX - fit.px) / 2);
    }
    const JRESULT dr = jd_decomp(&jd, artOut, fit.scale);
    ok = (dr == JDR_OK);
    if (ok) {
      snprintf(s_status, sizeof(s_status), "ok %dx%d /%d",
               (int)jd.width, (int)jd.height, 1 << fit.scale);
    } else if (dr == JDR_INP && ctx.stop != ART_STOP_NONE) {
      // Say which way it ran out and how far it got. "input stream ended" on
      // its own cannot tell a dead socket from a slow one from a short file,
      // and those are three different things to go and fix.
      const char* why = ctx.stop == ART_STOP_CLOSED ? "closed"
                      : ctx.stop == ART_STOP_STALL  ? "stalled"
                                                    : "short file";
      snprintf(s_status, sizeof(s_status), "%s at %uk/%uk", why,
               (unsigned)(ctx.got / 1024), (unsigned)(ctx.declared / 1024));
    } else {
      snprintf(s_status, sizeof(s_status), "decode: %s", artJdErr((int)dr));
    }
  }
  free(work);
  return ok;
}

#if WITH_TETHER
namespace {
struct ArtBuf { uint8_t* p; uint32_t len, cap; };

bool artCollect(void* ctx, const uint8_t* data, uint16_t len) {
  ArtBuf* b = (ArtBuf*)ctx;
  if (b->len + len > b->cap) return false;    // over the cap: stop, do not grow
  memcpy(b->p + b->len, data, len);
  b->len += len;
  return true;
}

struct ArtFileSink { File f; uint32_t len; };

bool artToFile(void* ctx, const uint8_t* data, uint16_t len) {
  ArtFileSink* s = (ArtFileSink*)ctx;
  if (s->len + len > ART_MAX_BYTES) return false;
  if (s->f.write(data, len) != len) return false;
  s->len += len;
  return true;
}
}  // namespace

// Over the cable the bytes are pushed at us, and a pull-driven decoder cannot
// be fed that way without a second task to block in. So the cover is held
// first — but a 40 KB single allocation never fit this cube's fragmented
// heap ("no heap for a cover, blk 35k" from the field, on a heap whose
// largest block has hovered near 33 KB all along). The stage is 24 KB now,
// which covers Spotify's 15-25 KB reality and fits the blocks this heap
// actually has; a bigger cover falls back to staging through a flash file,
// the same trick the rain radar lives by.
#define ART_RAM_STAGE 24576
#define ART_TMP_PATH  "/aa_t.tmp"

static bool artFetchToFile(const char* url) {
  ArtFileSink s;
  s.f = LittleFS.open(ART_TMP_PATH, "w");
  s.len = 0;
  if (!s.f) return false;
  const NetFetchResult r = netFetch(url, false, "Accept: image/jpeg", nullptr, 0,
                                    artToFile, &s, 20000);
  s.f.close();
  const bool ok = r.ok && s.len && r.bytes <= s.len;
  if (!ok) {
    snprintf(s_status, sizeof(s_status), "tether: %.28s",
             r.error[0] ? r.error : "cover truncated");
    LittleFS.remove(ART_TMP_PATH);
  }
  return ok;
}

static bool albumArtDrawTethered(const char* url, int16_t x, int16_t y) {
  // The common case: the whole cover in one RAM stage, decoded from memory.
  uint8_t* buf = (uint8_t*)malloc(ART_RAM_STAGE);
  if (buf) {
    ArtBuf b{buf, 0, ART_RAM_STAGE};
    const NetFetchResult r = netFetch(url, false, "Accept: image/jpeg", nullptr, 0,
                                      artCollect, &b, 15000);
    const bool overflow = r.bytes > b.len || b.len >= ART_RAM_STAGE;
    if (r.ok && b.len && !overflow) {
      ArtCtx ctx = {};
      ctx.mem = buf;
      ctx.memLen = b.len;
      ctx.declared = b.len;
      ctx.ox = x;
      ctx.oy = y;
      ctx.magicLen = 2;
      ctx.magic[0] = buf[0];
      ctx.magic[1] = b.len > 1 ? buf[1] : 0;
      const bool ok = artDecode(ctx, x, y);
      free(buf);
      s_retryAt = ok ? 0 : millis() + 15000;
      return ok;
    }
    free(buf);
    if (!overflow) {
      snprintf(s_status, sizeof(s_status), "tether: %.28s", r.error);
      s_retryAt = millis() + 15000;
      return false;
    }
    // A cover bigger than the stage: stage it through flash instead.
  }

  if (!artFetchToFile(url)) {
    s_retryAt = millis() + 15000;
    return false;
  }
  File f = LittleFS.open(ART_TMP_PATH, "r");
  bool ok = false;
  if (f) {
    ArtCtx ctx = {};
    ctx.file = &f;
    ctx.declared = f.size();
    ctx.ox = x;
    ctx.oy = y;
    ok = artDecode(ctx, x, y);
    f.close();
  } else {
    strlcpy(s_status, "fs: cover went missing", sizeof(s_status));
  }
  LittleFS.remove(ART_TMP_PATH);
  s_retryAt = ok ? 0 : millis() + 15000;
  return ok;
}
#endif  // WITH_TETHER

bool albumArtDraw(const char* url, int16_t x, int16_t y) {
  if (!url || !url[0]) { strlcpy(s_status, "no art url", sizeof(s_status)); return false; }

  // Every repaint used to retry a failed cover immediately, so one failure
  // became a fetch storm: each attempt burned a socket and a TLS arena, which
  // made the next attempt likelier to fail, which caused another repaint. The
  // screen went from working art to permanently broken in a few seconds. Fail
  // fast until the backoff expires.
  if (s_retryAt && (int32_t)(millis() - s_retryAt) < 0) return false;

  const uint32_t heap = ESP.getFreeHeap();
  if (heap < ART_MIN_HEAP) {
    snprintf(s_status, sizeof(s_status), "heap %u, need %u", (unsigned)heap,
             (unsigned)ART_MIN_HEAP);
    s_retryAt = millis() + 15000;
    return false;
  }

  // The poll task can be mid-handshake on core 0 right now, and two TLS
  // sessions at once do not fit in this heap. Wait for it rather than fail.
  if (!spotifyNetLock(2500)) {
    strlcpy(s_status, "busy: poll holds the radio", sizeof(s_status));
    // Nothing was spent here — no socket, no TLS arena, the poll just had the
    // radio at the wrong moment. It lets go within a few seconds, so the full
    // 15 s penalty was pure bad luck compounding: lose one race, sit out three.
    s_retryAt = millis() + 4000;
    return false;
  }
  struct NetRelease { ~NetRelease() { spotifyNetUnlock(); } } netRelease;

#if WITH_MINER
  // The hash workers hold the SHA peripheral that mbedTLS needs and share the
  // core with this task at the same priority. Between them they were failing
  // the handshake outright — "HTTP -1" with the miner on, a clean cover with it
  // off. Park them for the second this takes.
  minerCorePause();
  struct MinerRelease { ~MinerRelease() { minerCoreResume(); } } minerRelease;
#endif

#if WITH_TETHER
  if (netFetchTethered()) return albumArtDrawTethered(url, x, y);
#endif

  WiFiClientSecure client;
  client.setInsecure();          // same posture as the rest of the Spotify path
  client.setTimeout(6);
  HTTPClient http;
  http.setTimeout(6000);
  // The CDN can answer with a redirect, and HTTPClient does not follow one
  // unless asked — which looks identical to the image simply not loading.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  // Same reason StockClient does this, and this file had missed it: the decoder
  // reads the raw socket through getStreamPtr(), and neither core de-chunks, so
  // an HTTP/1.1 server answering chunked feeds chunk headers into the JPEG.
  // It also turns keep-alive off, which matters more here — with reuse on,
  // HTTPClient::end() deliberately leaves the socket OPEN for a reuse that can
  // never happen, because the client and the request are both locals destroyed
  // on the next line. Every cover leaked a TLS socket that way, which is why
  // the third one in a row could no longer get a clean stream.
  http.useHTTP10(true);
  http.setReuse(false);
  if (!http.begin(client, url)) {
    snprintf(s_status, sizeof(s_status), "begin failed, blk %uk",
             (unsigned)(platformMaxFreeBlock() / 1024));
    s_retryAt = millis() + 15000;
    return false;
  }

  // Say what we can actually decode. A CDN doing content negotiation will
  // otherwise happily hand back WebP, which the ROM decoder cannot read and
  // which looks from here like a corrupt JPEG.
  http.addHeader("Accept", "image/jpeg");

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(s_status, sizeof(s_status), "HTTP %d, blk %uk", code,
             (unsigned)(platformMaxFreeBlock() / 1024));
    http.end();
    s_retryAt = millis() + 15000;
    return false;
  }

  ArtCtx ctx = {};                         // mem/memLen/memPos must read as unset
  ctx.stream    = http.getStreamPtr();
  ctx.remaining = http.getSize();          // -1 when the server sent no length
  ctx.declared  = ctx.remaining > 0 ? (uint32_t)ctx.remaining : 0;
  ctx.ox = x;
  ctx.oy = y;
  ctx.deadline = millis() + ART_STALL_MS;

  const bool ok = artDecode(ctx, x, y);
  http.end();
  // 15 s is long enough that a spiral cannot form and short enough that a
  // transient failure fixes itself within one track.
  s_retryAt = ok ? 0 : millis() + 15000;
  return ok;
}

#endif  // WITH_SPOTIFY
