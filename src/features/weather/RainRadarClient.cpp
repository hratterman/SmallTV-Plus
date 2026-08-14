#include "RainRadarClient.h"
#if WITH_WEATHER

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp32/rom/tjpgd.h>
#include <miniz.h>              // the ROM inflater: tinfl_* costs no flash
#include "NetFetch.h"
#include "Platform.h"

#if WITH_SPOTIFY
#include "../spotify/SpotifyClient.h"   // spotifyNetLock: one TLS op at a time
#endif
#if WITH_MINER
#include "../miner/MinerCore.h"         // park the SHA workers while we build
#endif

#define RR_INDEX_URL "https://api.rainviewer.com/public/weather-maps.json"
#define RR_MAP_URL \
  "https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/%d/%d/%d"

#define RR_PNG_CAP   32768      // a z7 radar tile ran 7 KB in a Florida storm
#define RR_JPEG_CAP  40960      // the street tile ran 15 KB
#define RR_CYCLE_MS  600000UL   // RainViewer regenerates every ten minutes
#define RR_RETRY_MS  120000UL

// ---------------------------------------------------------------------------
// Shared state. The weather task writes, the display loop reads; s_ready flips
// false under the lock before any buffer is touched, so the loop can never
// draw from a half-rebuilt frame set.
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_lock = nullptr;
static volatile bool s_ready = false;

static uint8_t* s_map   = nullptr;      // RR_MAP_PX^2 RGB332
static uint8_t* s_grids = nullptr;      // RR_FRAMES_MAX * RR_GRID_BYTES
static uint8_t  s_frames = 0, s_nowIdx = 0;
static int16_t  s_minOff[RR_FRAMES_MAX];
static uint8_t  s_markerX = 0, s_markerY = 0;

static int  s_mapTileX = -1, s_mapTileY = -1;   // which tile s_map holds
static char s_note[52] = "radar: not tried";
static uint32_t s_nextCycleMs = 0;
static float s_lastLat = 0, s_lastLon = 0;

static inline void lockTake() { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void lockGive() { if (s_lock) xSemaphoreGive(s_lock); }

bool rainRadarReady() { return s_ready; }
const char* rainRadarNote() { return s_note; }

bool rainRadarAcquire(RainRadarView& v) {
  if (!s_lock) return false;
  lockTake();
  if (!s_ready) { lockGive(); return false; }
  v.frames  = s_frames;
  v.nowIdx  = s_nowIdx;
  v.markerX = s_markerX;
  v.markerY = s_markerY;
  v.map     = s_map;
  for (uint8_t i = 0; i < s_frames; i++) {
    v.minOff[i] = s_minOff[i];
    v.grid[i]   = s_grids + (size_t)i * RR_GRID_BYTES;
  }
  return true;   // caller releases
}

void rainRadarRelease() { lockGive(); }

static void rrTeardown(const char* why) {
  if (s_ready || s_map || s_grids) {
    lockTake();
    s_ready = false;
    lockGive();
    free(s_map);   s_map = nullptr;
    free(s_grids); s_grids = nullptr;
    s_mapTileX = s_mapTileY = -1;
    s_frames = 0;
  }
  if (why) strlcpy(s_note, why, sizeof(s_note));
}

// ---------------------------------------------------------------------------
// Fetch plumbing: collect a bounded binary body.
// ---------------------------------------------------------------------------
namespace {
struct RRBuf { uint8_t* p; uint32_t len, cap; };

bool rrCollect(void* ctx, const uint8_t* data, uint16_t len) {
  RRBuf* b = (RRBuf*)ctx;
  if (b->len + len > b->cap) return false;
  memcpy(b->p + b->len, data, len);
  b->len += len;
  return true;
}

bool rrFetch(const char* url, RRBuf& b, const char* accept, char* err, size_t errLen) {
#if WITH_SPOTIFY
  if (!spotifyNetLock(10000)) {
    strlcpy(err, "busy: poll holds the radio", errLen);
    return false;
  }
#endif
  const NetFetchResult r = netFetch(url, false, accept, nullptr, 0, rrCollect, &b, 15000);
#if WITH_SPOTIFY
  spotifyNetUnlock();
#endif
  if (!r.ok) {
    snprintf(err, errLen, "%.44s", r.error);
    return false;
  }
  if (!b.len) {
    strlcpy(err, "empty reply", errLen);
    return false;
  }
  return true;
}
}  // namespace

// ---------------------------------------------------------------------------
// Radar tile -> 64x64 nibble grid. The PNG is inflated with the ROM miniz
// (32 KB dictionary, streamed) and defiltered scanline by scanline; nothing
// the size of the decoded image ever exists.
// ---------------------------------------------------------------------------
namespace {
// One allocation for everything the inflater needs besides its dictionary.
struct RRInflate {
  tinfl_decompressor inf;
  uint8_t cur[1 + RR_TILE_PX * 4];   // filter byte + one RGBA scanline
  uint8_t prev[RR_TILE_PX * 4];      // previous scanline, defiltered
};

// Decode a fetched PNG into `grid` (cleared first). Only the exact shape
// RainViewer serves is accepted: 256x256, 8-bit RGBA.
bool rrDecodePng(const uint8_t* png, uint32_t len, uint8_t* grid,
                 char* err, size_t errLen) {
  memset(grid, 0, RR_GRID_BYTES);
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (len < 45 || memcmp(png, kSig, 8) != 0) {
    strlcpy(err, "not a PNG", errLen);
    return false;
  }
  const uint32_t w = ((uint32_t)png[16] << 24) | ((uint32_t)png[17] << 16) |
                     ((uint32_t)png[18] << 8) | png[19];
  const uint32_t h = ((uint32_t)png[20] << 24) | ((uint32_t)png[21] << 16) |
                     ((uint32_t)png[22] << 8) | png[23];
  if (w != RR_TILE_PX || h != RR_TILE_PX || png[24] != 8 || png[25] != 6) {
    snprintf(err, errLen, "PNG shape %ux%u/%u/%u", (unsigned)w, (unsigned)h,
             png[24], png[25]);
    return false;
  }

  RRInflate* z = (RRInflate*)malloc(sizeof(RRInflate));
  uint8_t* dict = (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
  if (!z || !dict) {
    free(z);
    free(dict);
    snprintf(err, errLen, "no heap to inflate, blk %uk",
             (unsigned)(platformMaxFreeBlock() / 1024));
    return false;
  }
  tinfl_init(&z->inf);
  memset(z->prev, 0, sizeof(z->prev));
  RRScan scan = {z->cur, z->prev, grid, 0, 0, false};

  bool ok = false;
  size_t dictOfs = 0;
  uint32_t off = 8;
  // Walk the chunks; every IDAT payload is one continuous zlib stream.
  while (off + 12 <= len) {
    const uint32_t clen = ((uint32_t)png[off] << 24) | ((uint32_t)png[off + 1] << 16) |
                          ((uint32_t)png[off + 2] << 8) | png[off + 3];
    const bool idat = memcmp(png + off + 4, "IDAT", 4) == 0;
    const bool iend = memcmp(png + off + 4, "IEND", 4) == 0;
    if (off + 12 + clen > len) break;
    if (idat) {
      const uint8_t* in = png + off + 8;
      size_t left = clen;
      // Whether more compressed bytes follow this chunk (another IDAT later).
      bool more = false;
      for (uint32_t o2 = off + 12 + clen; o2 + 12 <= len; ) {
        const uint32_t l2 = ((uint32_t)png[o2] << 24) | ((uint32_t)png[o2 + 1] << 16) |
                            ((uint32_t)png[o2 + 2] << 8) | png[o2 + 3];
        if (memcmp(png + o2 + 4, "IDAT", 4) == 0) { more = true; }
        if (memcmp(png + o2 + 4, "IEND", 4) == 0) break;
        o2 += 12 + l2;
        if (more) break;
      }
      for (;;) {
        size_t inSz = left;
        size_t outSz = TINFL_LZ_DICT_SIZE - dictOfs;
        const tinfl_status st = tinfl_decompress(
            &z->inf, in, &inSz, dict, dict + dictOfs, &outSz,
            TINFL_FLAG_PARSE_ZLIB_HEADER |
                ((left > inSz || more) ? TINFL_FLAG_HAS_MORE_INPUT : 0));
        in += inSz;
        left -= inSz;
        if (outSz) rrScanConsume(scan, dict + dictOfs, outSz);
        dictOfs = (dictOfs + outSz) & (TINFL_LZ_DICT_SIZE - 1);
        if (st == TINFL_STATUS_DONE) { ok = true; break; }
        if (st < 0) break;                               // corrupt stream
        if (st == TINFL_STATUS_HAS_MORE_OUTPUT) continue; // flush before feeding
        if (!left) break;                                // await the next IDAT
      }
      if (ok) break;   // a failed stream just falls through to IEND below
    }
    if (iend) break;
    off += 12 + clen;
  }

  const bool complete = ok && !scan.bad && scan.y == RR_TILE_PX;
  free(dict);
  free(z);
  if (!complete) strlcpy(err, scan.bad ? "bad PNG filter" : "short PNG", errLen);
  return complete;
}

bool rrFetchFrame(const String& host, const String& path, int tx, int ty,
                  uint8_t* grid, char* err, size_t errLen) {
  uint8_t* buf = (uint8_t*)malloc(RR_PNG_CAP);
  if (!buf) {
    strlcpy(err, "no heap for a tile", errLen);
    return false;
  }
  char url[160];
  snprintf(url, sizeof(url), "%s%s/256/%d/%d/%d/0/0_0.png",
           host.c_str(), path.c_str(), RR_ZOOM, tx, ty);
  RRBuf b{buf, 0, RR_PNG_CAP};
  bool ok = rrFetch(url, b, "Accept: image/png", err, errLen);
  if (ok) ok = rrDecodePng(buf, b.len, grid, err, errLen);
  free(buf);
  return ok;
}

// ---------------------------------------------------------------------------
// Base map: one Esri street tile, ROM TJpgDec at 1/2 scale, stored dimmed.
// ---------------------------------------------------------------------------
struct RRJpeg { const uint8_t* p; uint32_t len, pos; };

UINT rrJpgIn(JDEC* jd, BYTE* buff, UINT n) {
  RRJpeg* j = (RRJpeg*)jd->device;
  const uint32_t left = j->len - j->pos;
  if (n > left) n = left;
  if (buff) memcpy(buff, j->p + j->pos, n);
  j->pos += n;
  return n;
}

UINT rrJpgOut(JDEC* jd, void* bitmap, JRECT* rect) {
  (void)jd;
  const uint8_t* src = (const uint8_t*)bitmap;
  const int w = rect->right - rect->left + 1;
  for (int y = rect->top; y <= rect->bottom; y++) {
    if (y >= RR_MAP_PX) break;
    uint8_t* dst = s_map + (size_t)y * RR_MAP_PX;
    for (int x = rect->left; x <= rect->right; x++) {
      if (x >= RR_MAP_PX) continue;
      const uint8_t* p = src + ((size_t)(y - rect->top) * w + (x - rect->left)) * 3;
      dst[x] = rr565to332dim(p[0], p[1], p[2]);
    }
  }
  return 1;
}

bool rrFetchMap(int tx, int ty, char* err, size_t errLen) {
  uint8_t* buf = (uint8_t*)malloc(RR_JPEG_CAP);
  if (!buf) {
    strlcpy(err, "no heap for the map", errLen);
    return false;
  }
  char url[160];
  snprintf(url, sizeof(url), RR_MAP_URL, RR_ZOOM, ty, tx);   // Esri is z/y/x
  RRBuf b{buf, 0, RR_JPEG_CAP};
  bool ok = rrFetch(url, b, "Accept: image/jpeg", err, errLen);
  if (ok) {
    void* work = malloc(4096);
    if (!work) {
      strlcpy(err, "no heap for the decoder", errLen);
      ok = false;
    } else {
      RRJpeg j{buf, b.len, 0};
      JDEC jd;
      JRESULT r = jd_prepare(&jd, rrJpgIn, work, 4096, &j);
      if (r == JDR_OK) r = jd_decomp(&jd, rrJpgOut, 1);      // 1/2 scale: 128x128
      if (r != JDR_OK) {
        snprintf(err, errLen, "map decode err %d", (int)r);
        ok = false;
      }
      free(work);
    }
  }
  free(buf);
  if (ok) { s_mapTileX = tx; s_mapTileY = ty; }
  return ok;
}
}  // namespace

// ---------------------------------------------------------------------------
// The cycle.
// ---------------------------------------------------------------------------
void rainRadarCycle(float lat, float lon, bool enabled) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (!enabled) {
    if (s_ready || s_map) rrTeardown("radar off");
    return;
  }
  const uint32_t now = millis();
  if (lat != s_lastLat || lon != s_lastLon) {
    s_lastLat = lat;
    s_lastLon = lon;
    rrTeardown(nullptr);          // a new place: old tiles are the wrong place
    s_nextCycleMs = 0;
  }
  if (s_nextCycleMs && (int32_t)(now - s_nextCycleMs) < 0) return;
  s_nextCycleMs = now + RR_RETRY_MS;   // pessimistic; success stretches it below

  char err[48] = "";

  // The build's biggest single pieces are 32 KB (the inflate dictionary and
  // the tile buffer); everything is allocation-checked, so the guard only has
  // to keep the attempt from starving a TLS handshake somewhere else. On a
  // cube running the miner the heap simply never looks comfortable, and the
  // first guard (100 KB free) turned out to mean "never" there.
  if (ESP.getFreeHeap() < 80000 || platformMaxFreeBlock() < 34000) {
    snprintf(s_note, sizeof(s_note), "radar: heap %uk blk %uk",
             (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(platformMaxFreeBlock() / 1024));
    return;
  }

#if WITH_MINER
  // Same reason the album art does it: the hash workers hold the SHA engine
  // mbedTLS wants and the core this task shares. A build is seconds long.
  minerCorePause();
  struct MinerRelease { ~MinerRelease() { minerCoreResume(); } } minerRelease;
#endif

  // 1. The frame index.
  String idx;
  {
#if WITH_SPOTIFY
    if (!spotifyNetLock(10000)) { strlcpy(s_note, "radar: radio busy", sizeof(s_note)); return; }
#endif
    const NetFetchResult r = netFetchToString(RR_INDEX_URL, false,
                                              "Accept: application/json",
                                              nullptr, 0, idx, 8192, 12000);
#if WITH_SPOTIFY
    spotifyNetUnlock();
#endif
    if (!r.ok) {
      snprintf(s_note, sizeof(s_note), "radar idx: %.32s", r.error);
      return;
    }
  }

  String host;
  String paths[RR_FRAMES_MAX];
  int16_t offs[RR_FRAMES_MAX];
  uint8_t total = 0, nowIdx = 0;
  {
    JsonDocument filter;
    filter["host"] = true;
    filter["radar"]["past"][0]["time"] = true;
    filter["radar"]["past"][0]["path"] = true;
    filter["radar"]["nowcast"][0]["time"] = true;
    filter["radar"]["nowcast"][0]["path"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, idx, DeserializationOption::Filter(filter)) !=
            DeserializationError::Ok ||
        !doc["radar"]["past"].is<JsonArrayConst>()) {
      strlcpy(s_note, "radar idx: bad JSON", sizeof(s_note));
      return;
    }
    host = doc["host"] | "";
    JsonArrayConst past = doc["radar"]["past"].as<JsonArrayConst>();
    const int pn = (int)past.size();
    if (!host.length() || pn == 0) {
      strlcpy(s_note, "radar idx: no frames", sizeof(s_note));
      return;
    }
    const int take = pn < RR_PAST_FRAMES ? pn : RR_PAST_FRAMES;
    const int64_t newest = past[pn - 1]["time"] | 0;
    for (int i = 0; i < take; i++) {
      JsonObjectConst f = past[pn - take + i];
      paths[total] = (const char*)(f["path"] | "");
      offs[total] = (int16_t)((((int64_t)(f["time"] | 0)) - newest) / 60);
      total++;
    }
    nowIdx = (uint8_t)(total - 1);
    JsonArrayConst cast = doc["radar"]["nowcast"].as<JsonArrayConst>();
    for (JsonObjectConst f : cast) {
      if (total >= RR_FRAMES_MAX) break;
      paths[total] = (const char*)(f["path"] | "");
      offs[total] = (int16_t)((((int64_t)(f["time"] | 0)) - newest) / 60);
      total++;
    }
  }

  const RRTile tile = rrTileForLatLon(lat, lon, RR_ZOOM);

  // 2. Buffers, then the cheap gate: the newest observed frame alone decides
  // whether anything else is worth fetching.
  if (!s_grids) s_grids = (uint8_t*)malloc((size_t)RR_FRAMES_MAX * RR_GRID_BYTES);
  if (!s_grids) {
    strlcpy(s_note, "radar: no heap for frames", sizeof(s_note));
    return;
  }
  lockTake();
  s_ready = false;              // buffers are about to churn
  lockGive();

  uint8_t* nowGrid = s_grids + (size_t)nowIdx * RR_GRID_BYTES;
  if (!rrFetchFrame(host, paths[nowIdx], tile.x, tile.y, nowGrid,
                    err, sizeof(err))) {
    snprintf(s_note, sizeof(s_note), "radar: %.40s", err);
    return;
  }
  if (rrGridActive(nowGrid) < RR_GATE_CELLS) {
    rrTeardown("radar quiet");
    s_nextCycleMs = now + RR_CYCLE_MS;
    return;
  }

  // 3. Something out there. The map (cached until the tile changes)...
  if (!s_map) s_map = (uint8_t*)malloc((size_t)RR_MAP_PX * RR_MAP_PX);
  if (!s_map) {
    strlcpy(s_note, "radar: no heap for the map", sizeof(s_note));
    return;
  }
  if (s_mapTileX != tile.x || s_mapTileY != tile.y) {
    if (!rrFetchMap(tile.x, tile.y, err, sizeof(err))) {
      snprintf(s_note, sizeof(s_note), "radar map: %.38s", err);
      return;
    }
  }

  // 4. ...then the rest of the hour, oldest first.
  for (uint8_t i = 0; i < total; i++) {
    if (i == nowIdx) continue;
    if (!rrFetchFrame(host, paths[i], tile.x, tile.y,
                      s_grids + (size_t)i * RR_GRID_BYTES, err, sizeof(err))) {
      snprintf(s_note, sizeof(s_note), "radar f%u: %.38s", i, err);
      return;
    }
  }

  // 5. Publish.
  lockTake();
  s_frames = total;
  s_nowIdx = nowIdx;
  for (uint8_t i = 0; i < total; i++) s_minOff[i] = offs[i];
  s_markerX = tile.px;
  s_markerY = tile.py;
  s_ready = true;
  lockGive();
  snprintf(s_note, sizeof(s_note), "radar: %u frames", total);
  s_nextCycleMs = now + RR_CYCLE_MS;
}

#endif  // WITH_WEATHER
