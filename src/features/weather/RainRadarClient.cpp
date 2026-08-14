#include "RainRadarClient.h"
#if WITH_WEATHER

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp32/rom/tjpgd.h>
extern "C" {
#include "../../vendor/uzlib/uzlib.h"   // ~1.4 KB of state where ROM miniz wants ~14
}
#include "NetFetch.h"
#include "Platform.h"

#if WITH_SPOTIFY
#include "../spotify/SpotifyClient.h"   // spotifyNetLock: one TLS op at a time
#endif
#if WITH_MINER
#include "../miner/MinerCore.h"         // park the SHA workers while we build
#endif

// Tiles ride plain HTTP on WiFi (measured: both RainViewer hosts answer it),
// which keeps the TLS arena — and the TLS lock — out of the radar's way
// entirely. Down the tether the browser does the TLS, so https costs nothing
// there and mixed-content rules require it anyway. Esri only answers TLS.
#define RR_IDX_FMT  "%s://api.rainviewer.com/public/weather-maps.json"
#define RR_TILE_FMT "%s%s/256/%d/%d/%d/0/0_0.png"
#define RR_MAP_URL \
  "https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/%d/%d/%d"

#define RR_CYCLE_MS  600000UL   // RainViewer regenerates every ten minutes
#define RR_RETRY_MS  120000UL   // after a real failure
#define RR_BUSY_MS   12000UL    // after losing the radio: it frees in seconds
// A z7 tile with a whole storm system across it compresses far worse than a
// lone cell cluster: the field cube met one past 40 KB, whose truncation the
// decoder then reported as "short PNG" every cycle. The FS partition is
// ~960 KB; 96 KB of cap costs nothing and covers the honest worst case.
#define RR_FILE_CAP  98304

// Flash layout, all in the FS root: the grid files are keyed by the frame's
// timestamp, which is what makes a steady rain cost one fetch per cycle —
// last cycle's frames are already on disk under their own names.
#define RR_TMP_PATH  "/rr_t.tmp"
#define RR_GRID_FMT  "/rr_g%u.bin"
#define RR_MAP_FMT   "/rr_m_%d_%d.bin"

// ---------------------------------------------------------------------------
// Shared state. The weather task writes, the display loop reads; s_ready
// flips false under the lock before any file is touched, so the renderer can
// never stream a half-rebuilt frame set.
// ---------------------------------------------------------------------------
static SemaphoreHandle_t s_lock = nullptr;
static volatile bool s_ready = false;

static uint8_t  s_frames = 0, s_nowIdx = 0;
static uint32_t s_ts[RR_FRAMES_MAX];            // frame timestamps = file keys
static int16_t  s_minOff[RR_FRAMES_MAX];
static uint8_t  s_markerX = 0, s_markerY = 0;
static int      s_mapTileX = -1, s_mapTileY = -1;

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
  for (uint8_t i = 0; i < s_frames; i++) v.minOff[i] = s_minOff[i];
  return true;   // caller releases
}

void rainRadarRelease() { lockGive(); }

bool rainRadarReadGrid(uint8_t frameIdx, uint8_t* out) {
  if (frameIdx >= s_frames) return false;
  char path[24];
  snprintf(path, sizeof(path), RR_GRID_FMT, (unsigned)s_ts[frameIdx]);
  File f = LittleFS.open(path, "r");
  const bool ok = f && f.read(out, RR_GRID_BYTES) == RR_GRID_BYTES;
  if (f) f.close();
  return ok;
}

static File s_mapFile;                 // open only between mapBegin/mapEnd
static int  s_mapFileRow = -1;

bool rainRadarMapBegin() {
  char path[28];
  snprintf(path, sizeof(path), RR_MAP_FMT, s_mapTileX, s_mapTileY);
  s_mapFile = LittleFS.open(path, "r");
  s_mapFileRow = -1;
  return (bool)s_mapFile;
}

bool rainRadarMapRow(int mapRow, uint8_t* out) {
  if (!s_mapFile || mapRow < 0 || mapRow >= RR_MAP_PX) return false;
  // The renderer walks rows in order; seek only when it doesn't.
  if (mapRow != s_mapFileRow + 1) s_mapFile.seek((size_t)mapRow * RR_MAP_PX);
  s_mapFileRow = mapRow;
  return s_mapFile.read(out, RR_MAP_PX) == RR_MAP_PX;
}

void rainRadarMapEnd() {
  if (s_mapFile) s_mapFile.close();
  s_mapFileRow = -1;
}

// ---------------------------------------------------------------------------
// Housekeeping: everything the radar keeps on flash carries the rr_ prefix.
// ---------------------------------------------------------------------------
static bool rrIsOurs(const char* name) {
  // Some cores hand back "/rr_..." and some "rr_...": accept either.
  if (name[0] == '/') name++;
  return strncmp(name, "rr_", 3) == 0;
}

// Delete every rr_ file that `keep` does not bless. keep == nullptr: all go.
static void rrSweep(bool (*keep)(const char* name)) {
  File root = LittleFS.open("/");
  if (!root) return;
  char doomed[24][28];
  int n = 0;
  for (File f = root.openNextFile(); f && n < 24; f = root.openNextFile()) {
    const char* nm = f.name();
    if (!rrIsOurs(nm)) continue;
    if (keep && keep(nm)) continue;
    snprintf(doomed[n], sizeof(doomed[n]), "%s%s", nm[0] == '/' ? "" : "/", nm);
    n++;
  }
  root.close();
  for (int i = 0; i < n; i++) LittleFS.remove(doomed[i]);
}

static void rrTeardown(const char* why) {
  lockTake();
  s_ready = false;
  s_frames = 0;
  lockGive();
  s_mapTileX = s_mapTileY = -1;
  rrSweep(nullptr);
  if (why) strlcpy(s_note, why, sizeof(s_note));
}

// ---------------------------------------------------------------------------
// The shared TLS radio, taken politely — and only for TLS. Plain-HTTP tile
// fetches need no big heap and skip the queue entirely, which is most of why
// the "radar: radio busy" era ended. Three long attempts; losing reschedules
// the next look for seconds away, because the radio frees in seconds.
// ---------------------------------------------------------------------------
static bool rrNetLock() {
#if WITH_SPOTIFY
  for (int a = 0; a < 3; a++) {
    if (spotifyNetLock(8000)) return true;
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
  s_nextCycleMs = millis() + RR_BUSY_MS;
  return false;
#else
  return true;
#endif
}

static void rrNetUnlock() {
#if WITH_SPOTIFY
  spotifyNetUnlock();
#endif
}

// ---------------------------------------------------------------------------
// Fetch: the body streams straight into a flash file, no RAM in between.
// ---------------------------------------------------------------------------
namespace {
struct RRFileSink { File f; uint32_t len; };

bool rrToFile(void* ctx, const uint8_t* data, uint16_t len) {
  RRFileSink* s = (RRFileSink*)ctx;
  if (s->len + len > RR_FILE_CAP) return false;
  if (s->f.write(data, len) != len) return false;   // FS full or dying
  s->len += len;
  return true;
}

// Fetch url into RR_TMP_PATH. `tls` says whether this URL costs a TLS arena
// and must therefore queue for the radio.
bool rrFetchToTmp(const char* url, bool tls, char* err, size_t errLen) {
  if (tls && !rrNetLock()) {
    strlcpy(err, "radio busy", errLen);
    return false;
  }
  RRFileSink s;
  s.f = LittleFS.open(RR_TMP_PATH, "w");
  s.len = 0;
  bool ok = false;
  if (!s.f) {
    strlcpy(err, "fs: cannot write", errLen);
  } else {
    const NetFetchResult r = netFetch(url, false, nullptr, nullptr, 0,
                                      rrToFile, &s, 20000);
    s.f.close();
    // Cap first: a sink that stopped accepting still leaves r.ok true (the
    // status was 200), and accepting that file hands the decoder a stream
    // with its tail cut off. That was a real bug, found as "short PNG".
    if (s.len >= RR_FILE_CAP) snprintf(err, errLen, "tile over %uk", RR_FILE_CAP / 1024);
    else if (r.ok && s.len) ok = true;
    else if (!r.ok) snprintf(err, errLen, "%.44s", r.error);
    else strlcpy(err, "empty reply", errLen);
  }
  if (tls) rrNetUnlock();
  if (!ok) LittleFS.remove(RR_TMP_PATH);
  return ok;
}

// ---------------------------------------------------------------------------
// Radar tile -> 64x64 nibble grid, streamed from the flash file. uzlib
// inflates with the unavoidable 32 KB dictionary but only ~1.4 KB of its own
// state - the ROM miniz wanted ~14 KB more, which on a cube reporting 48 KB
// free was the difference between running and refusing. The PNG chunk walk
// lives inside the input callback: uzlib asks for bytes, and the callback
// serves IDAT payloads off the file, skipping headers and CRCs as it goes.
// ---------------------------------------------------------------------------
// The context stays small (~2 KB) and the scanline buffers are allocated as
// separate kilobyte pieces, because of a lesson the field cube taught in one
// line ("no heap to inflate, blk 33k"): after the 32 KB dictionary takes the
// heap's one big block, a further 6 KB piece may not exist anywhere — the
// rest of a 48 KB heap is crumbs. Several small allocations fit where one
// medium one cannot.
struct RRUz {
  struct uzlib_uncomp u;             // FIRST: the read callback containerofs it
  File*    f;
  uint32_t chunkLeft;                // bytes left in the current IDAT payload
  bool     sawEnd;
  uint8_t  zh[2];                    // first two bytes served: the zlib header
  bool     zhSeen;                   // (forensics for the failure note)
  uint8_t  in[512];                  // compressed bytes, read from flash
};

int rrUzRead(struct uzlib_uncomp* u) {
  RRUz* s = (RRUz*)u;                // u is the first member
  while (s->chunkLeft == 0) {
    if (s->sawEnd) return -1;
    // Skip the previous chunk's CRC, then read the next chunk's header.
    uint8_t ch[8];
    s->f->seek(s->f->position() + 4);
    if (s->f->read(ch, 8) != 8) { s->sawEnd = true; return -1; }
    const uint32_t clen = ((uint32_t)ch[0] << 24) | ((uint32_t)ch[1] << 16) |
                          ((uint32_t)ch[2] << 8) | ch[3];
    if (memcmp(ch + 4, "IEND", 4) == 0) { s->sawEnd = true; return -1; }
    if (memcmp(ch + 4, "IDAT", 4) != 0) {
      s->f->seek(s->f->position() + clen);   // its CRC goes on the next loop
      continue;
    }
    s->chunkLeft = clen;
  }
  size_t want = sizeof(s->in);
  if (want > s->chunkLeft) want = s->chunkLeft;
  const int got = s->f->read(s->in, want);
  if (got <= 0) { s->sawEnd = true; return -1; }
  s->chunkLeft -= (uint32_t)got;
  if (!s->zhSeen) {
    s->zh[0] = s->in[0];
    s->zh[1] = got > 1 ? s->in[1] : 0;
    s->zhSeen = true;
  }
  u->source = s->in + 1;
  u->source_limit = s->in + got;
  return s->in[0];
}

bool rrDecodeTmp(uint8_t* grid, char* err, size_t errLen) {
  memset(grid, 0, RR_GRID_BYTES);
  File f = LittleFS.open(RR_TMP_PATH, "r");
  if (!f) {
    strlcpy(err, "fs: tile went missing", errLen);
    return false;
  }

  // 29 bytes: signature(8) + IHDR length(4) + type(4) + payload(13). NOT the
  // CRC too - reading 33 left the file at the next chunk's header, and the
  // callback's skip-the-CRC step then landed the inflater four bytes inside
  // it, misreading "IDAT" as a length. That one-byte arithmetic slip was
  // "PNG data error y=0" in the field, and the selftest's chunk-walk replica
  // now fails loudly if it ever comes back.
  uint8_t hdr[29];
  bool shapeOk = f.read(hdr, 29) == 29;
  static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (shapeOk) shapeOk = memcmp(hdr, kSig, 8) == 0;
  if (shapeOk) {
    const uint32_t w = ((uint32_t)hdr[16] << 24) | ((uint32_t)hdr[17] << 16) |
                       ((uint32_t)hdr[18] << 8) | hdr[19];
    const uint32_t h = ((uint32_t)hdr[20] << 24) | ((uint32_t)hdr[21] << 16) |
                       ((uint32_t)hdr[22] << 8) | hdr[23];
    // Only the exact shape RainViewer serves: 256x256, 8-bit RGBA.
    shapeOk = w == RR_TILE_PX && h == RR_TILE_PX && hdr[24] == 8 && hdr[25] == 6;
    if (!shapeOk)
      snprintf(err, errLen, "PNG shape %ux%u/%u/%u", (unsigned)w, (unsigned)h,
               hdr[24], hdr[25]);
  } else {
    strlcpy(err, "not a PNG", errLen);
  }
  if (!shapeOk) {
    f.close();
    return false;
  }
  // The file now sits ON IHDR's CRC; the read callback's first act is to
  // skip those four bytes and find the first IDAT.

  // Biggest first, so the note names the dictionary if the heap cannot seat
  // it; then the small pieces, which live in the crumbs.
  uint8_t* dict = (uint8_t*)malloc(32768);
  RRUz*    z    = (RRUz*)malloc(sizeof(RRUz));
  uint8_t* out  = (uint8_t*)malloc(1024);
  uint8_t* cur  = (uint8_t*)malloc(1 + RR_TILE_PX * 4);
  uint8_t* prev = (uint8_t*)malloc(RR_TILE_PX * 4);
  if (!dict || !z || !out || !cur || !prev) {
    free(prev);
    free(cur);
    free(out);
    free(z);
    free(dict);
    f.close();
    snprintf(err, errLen, "no heap to inflate, %uk blk %uk",
             (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(platformMaxFreeBlock() / 1024));
    return false;
  }
  memset(&z->u, 0, sizeof(z->u));
  z->f = &f;
  z->chunkLeft = 0;
  z->sawEnd = false;
  z->zhSeen = false;
  z->zh[0] = z->zh[1] = 0;
  const uint32_t fileKb = (uint32_t)(f.size() / 1024);
  memset(prev, 0, RR_TILE_PX * 4);
  z->u.source = z->u.source_limit = nullptr;
  z->u.source_read_cb = rrUzRead;
  uzlib_uncompress_init(&z->u, dict, 32768);
  RRScan scan = {cur, prev, grid, 0, 0, false};

  bool done = false, corrupt = false;
  int stage = 0;                       // 0: died in the zlib header; 1: after
  if (uzlib_zlib_parse_header(&z->u) < 0) {
    corrupt = true;
  } else {
    stage = 1;
    while (!done && !corrupt && !scan.bad) {
      z->u.dest_start = z->u.dest = out;
      z->u.dest_limit = out + 1024;
      const int res = uzlib_uncompress(&z->u);
      const size_t got = (size_t)(z->u.dest - out);
      if (got) rrScanConsume(scan, out, got);
      if (res == TINF_DONE) done = true;
      else if (res != TINF_OK) corrupt = true;
      else if (!got) corrupt = true;         // no progress: a wedged stream
    }
  }
  const uint8_t z2h0 = z->zh[0], z2h1 = z->zh[1];
  f.close();
  free(prev);
  free(cur);
  free(out);
  free(z);
  free(dict);

  const bool complete = done && !scan.bad && scan.y == RR_TILE_PX;
  if (!complete) {
    // Everything a remote diagnosis needs on one line: which stage died (e0 =
    // the zlib header itself, e1 = mid-stream), how many scanlines arrived,
    // the file's size, and the two bytes the inflater was handed as the zlib
    // header. 78 9C there says the walk found a real stream; anything else
    // says the file is not what the tile server serves.
    if (scan.bad)
      snprintf(err, errLen, "PNG filter y=%u", scan.y);
    else if (corrupt)
      snprintf(err, errLen, "PNG e%d y=%u %uk %02X%02X", stage, scan.y,
               (unsigned)fileKb, z2h0, z2h1);
    else
      snprintf(err, errLen, "short PNG y=%u %uk", scan.y, (unsigned)fileKb);
  }
  return complete;
}

// Fetch + decode one radar frame; on rain, persist its grid under `ts`.
bool rrBuildFrame(const char* host, const char* path, int tx, int ty,
                  bool tls, uint32_t ts, uint8_t* grid, char* err, size_t errLen) {
  char url[160];
  snprintf(url, sizeof(url), RR_TILE_FMT, host, path, RR_ZOOM, tx, ty);
  if (!rrFetchToTmp(url, tls, err, errLen)) return false;
  const bool ok = rrDecodeTmp(grid, err, errLen);
  LittleFS.remove(RR_TMP_PATH);
  if (!ok) return false;
  char gp[24];
  snprintf(gp, sizeof(gp), RR_GRID_FMT, (unsigned)ts);
  File g = LittleFS.open(gp, "w");
  if (!g || g.write(grid, RR_GRID_BYTES) != RR_GRID_BYTES) {
    if (g) g.close();
    LittleFS.remove(gp);
    strlcpy(err, "fs: grid write failed", errLen);
    return false;
  }
  g.close();
  return true;
}

// ---------------------------------------------------------------------------
// Base map: one Esri street tile (TLS-only host), decoded from flash by the
// ROM TJpgDec at 1/2 scale into a 16 KB staging buffer, dimmed to RGB332,
// written back to flash, freed. The renderer never sees it in RAM again.
// ---------------------------------------------------------------------------
struct RRJpeg { File* f; uint8_t* out; };

UINT rrJpgIn(JDEC* jd, BYTE* buff, UINT n) {
  RRJpeg* j = (RRJpeg*)jd->device;
  if (buff) return (UINT)j->f->read(buff, n);
  j->f->seek(j->f->position() + n);
  return n;
}

UINT rrJpgOut(JDEC* jd, void* bitmap, JRECT* rect) {
  RRJpeg* j = (RRJpeg*)jd->device;
  const uint8_t* src = (const uint8_t*)bitmap;
  const int w = rect->right - rect->left + 1;
  for (int y = rect->top; y <= rect->bottom; y++) {
    if (y >= RR_MAP_PX) break;
    uint8_t* dst = j->out + (size_t)y * RR_MAP_PX;
    for (int x = rect->left; x <= rect->right; x++) {
      if (x >= RR_MAP_PX) continue;
      const uint8_t* p = src + ((size_t)(y - rect->top) * w + (x - rect->left)) * 3;
      dst[x] = rr565to332dim(p[0], p[1], p[2]);
    }
  }
  return 1;
}

bool rrBuildMap(int tx, int ty, char* err, size_t errLen) {
  char url[160];
  snprintf(url, sizeof(url), RR_MAP_URL, RR_ZOOM, ty, tx);   // Esri is z/y/x
  if (!rrFetchToTmp(url, true, err, errLen)) return false;

  bool ok = false;
  uint8_t* stage = (uint8_t*)malloc((size_t)RR_MAP_PX * RR_MAP_PX);
  void* work = stage ? malloc(4096) : nullptr;
  if (!stage || !work) {
    strlcpy(err, "no heap for the map", errLen);
  } else {
    File f = LittleFS.open(RR_TMP_PATH, "r");
    if (!f) {
      strlcpy(err, "fs: map went missing", errLen);
    } else {
      RRJpeg j{&f, stage};
      JDEC jd;
      JRESULT r = jd_prepare(&jd, rrJpgIn, work, 4096, &j);
      if (r == JDR_OK) r = jd_decomp(&jd, rrJpgOut, 1);      // 1/2 scale: 128x128
      f.close();
      if (r != JDR_OK) {
        snprintf(err, errLen, "map decode err %d", (int)r);
      } else {
        char mp[28];
        snprintf(mp, sizeof(mp), RR_MAP_FMT, tx, ty);
        File m = LittleFS.open(mp, "w");
        if (m && m.write(stage, (size_t)RR_MAP_PX * RR_MAP_PX) ==
                     (size_t)RR_MAP_PX * RR_MAP_PX) {
          ok = true;
        } else {
          if (m) { m.close(); LittleFS.remove(mp); }
          strlcpy(err, "fs: map write failed", errLen);
        }
        if (ok) m.close();
      }
    }
  }
  free(work);
  free(stage);
  LittleFS.remove(RR_TMP_PATH);
  if (ok) { s_mapTileX = tx; s_mapTileY = ty; }
  return ok;
}

// The sweep's keep-list, built per cycle: current grid timestamps + the map.
uint32_t g_keepTs[RR_FRAMES_MAX];
uint8_t  g_keepN = 0;
int      g_keepMapX = -1, g_keepMapY = -1;

// "1786661400" out of a filename, without sscanf (16 KB of flash this
// codebase already evicted once). Advances p past the digits.
bool rrParseUint(const char*& p, uint32_t& v) {
  if (*p < '0' || *p > '9') return false;
  v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (uint32_t)(*p++ - '0');
  return true;
}

bool rrKeepCurrent(const char* name) {
  if (name[0] == '/') name++;
  if (strncmp(name, "rr_g", 4) == 0) {
    const char* p = name + 4;
    uint32_t v;
    if (!rrParseUint(p, v) || strcmp(p, ".bin") != 0) return false;
    for (uint8_t i = 0; i < g_keepN; i++)
      if (g_keepTs[i] == v) return true;
    return false;
  }
  if (strncmp(name, "rr_m_", 5) == 0) {
    const char* p = name + 5;
    uint32_t x, y;
    if (!rrParseUint(p, x) || *p != '_') return false;
    p++;
    if (!rrParseUint(p, y) || strcmp(p, ".bin") != 0) return false;
    return (int)x == g_keepMapX && (int)y == g_keepMapY;
  }
  return false;   // tmp files and strangers are always swept
}
}  // namespace

// ---------------------------------------------------------------------------
// The cycle.
// ---------------------------------------------------------------------------
void rainRadarCycle(float lat, float lon, bool enabled) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (!enabled) {
    if (s_ready || s_mapTileX >= 0) rrTeardown("radar off");
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

  // The inflate transient is the whole heap story: a 32 KB dictionary in one
  // piece plus ~6 KB of uzlib state and buffers. The field cube this was
  // tuned against reports 48 KB free / 33 KB block, and must pass.
  if (ESP.getFreeHeap() < 42000 || platformMaxFreeBlock() < 33500) {
    snprintf(s_note, sizeof(s_note), "radar: heap %uk blk %uk",
             (unsigned)(ESP.getFreeHeap() / 1024),
             (unsigned)(platformMaxFreeBlock() / 1024));
    return;
  }
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < 220000) {
    snprintf(s_note, sizeof(s_note), "radar: fs %uk free",
             (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));
    return;
  }

#if WITH_MINER
  // Same reason the album art does it: the hash workers hold the SHA engine
  // mbedTLS wants and the core this task shares. A build is seconds long.
  minerCorePause();
  struct MinerRelease { ~MinerRelease() { minerCoreResume(); } } minerRelease;
#endif

  const bool tether = netFetchTethered();
  const char* scheme = tether ? "https" : "http";

  // 1. The frame index. Plain HTTP on WiFi — no TLS arena, no radio queue.
  String idx;
  {
    char url[80];
    snprintf(url, sizeof(url), RR_IDX_FMT, scheme);
    if (tether && !rrNetLock()) {
      strlcpy(s_note, "radar: radio busy", sizeof(s_note));
      return;
    }
    const NetFetchResult r = netFetchToString(url, false, "Accept: application/json",
                                              nullptr, 0, idx, 8192, 12000);
    if (tether) rrNetUnlock();
    if (!r.ok) {
      snprintf(s_note, sizeof(s_note), "radar idx: %.32s", r.error);
      return;
    }
  }

  String host;
  String paths[RR_FRAMES_MAX];
  uint32_t tss[RR_FRAMES_MAX];
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
    if (host.startsWith("https://") && !tether) {
      // The index names its tile host with a scheme; on WiFi we speak plain
      // HTTP to it (measured to answer), so swap the scheme in place.
      host = "http://" + host.substring(8);
    }
    JsonArrayConst past = doc["radar"]["past"].as<JsonArrayConst>();
    const int pn = (int)past.size();
    if (!host.length() || pn == 0) {
      strlcpy(s_note, "radar idx: no frames", sizeof(s_note));
      return;
    }
    const int take = pn < RR_PAST_FRAMES ? pn : RR_PAST_FRAMES;
    const int64_t newest = past[pn - 1]["time"] | 0;
    for (int i = 0; i < take; i++) {
      JsonObjectConst fr = past[pn - take + i];
      paths[total] = (const char*)(fr["path"] | "");
      tss[total] = (uint32_t)(fr["time"] | 0);
      offs[total] = (int16_t)((((int64_t)tss[total]) - newest) / 60);
      total++;
    }
    nowIdx = (uint8_t)(total - 1);
    JsonArrayConst cast = doc["radar"]["nowcast"].as<JsonArrayConst>();
    for (JsonObjectConst fr : cast) {
      if (total >= RR_FRAMES_MAX) break;
      paths[total] = (const char*)(fr["path"] | "");
      tss[total] = (uint32_t)(fr["time"] | 0);
      offs[total] = (int16_t)((((int64_t)tss[total]) - newest) / 60);
      total++;
    }
  }

  const RRTile tile = rrTileForLatLon(lat, lon, RR_ZOOM);
  // The per-frame scratch lives on the heap for the cycle: the task's stack
  // also has to carry a TLS handshake for the map fetch, and 2 KB of dormant
  // buffer under mbedTLS's frames is how stacks overflow politely.
  struct Scratch {
    uint8_t* p = (uint8_t*)malloc(RR_GRID_BYTES);
    ~Scratch() { free(p); }
  } sc;
  if (!sc.p) {
    strlcpy(s_note, "radar: no heap for scratch", sizeof(s_note));
    return;
  }
  uint8_t* grid = sc.p;

  // Files are about to churn; the renderer must let go first.
  lockTake();
  s_ready = false;
  lockGive();

  // 2. The cheap gate: the newest observed frame decides whether anything
  // else is worth fetching. Its grid may already be on flash from last cycle.
  {
    char gp[24];
    snprintf(gp, sizeof(gp), RR_GRID_FMT, (unsigned)tss[nowIdx]);
    File g = LittleFS.open(gp, "r");
    bool have = g && g.read(grid, RR_GRID_BYTES) == RR_GRID_BYTES;
    if (g) g.close();
    if (!have) {
      // Decode first, persist only if it turns out to be worth keeping — a
      // dry sky must not wear the flash.
      char url[160];
      snprintf(url, sizeof(url), RR_TILE_FMT, host.c_str(), paths[nowIdx].c_str(),
               RR_ZOOM, tile.x, tile.y);
      if (!rrFetchToTmp(url, tether, err, sizeof(err))) {
        snprintf(s_note, sizeof(s_note), "radar: %.40s", err);
        return;
      }
      const bool dec = rrDecodeTmp(grid, err, sizeof(err));
      LittleFS.remove(RR_TMP_PATH);
      if (!dec) {
        snprintf(s_note, sizeof(s_note), "radar: %.40s", err);
        return;
      }
    }
    if (rrGridActive(grid) < RR_GATE_CELLS) {
      g_keepN = 0;                     // sweep every stale grid away
      g_keepMapX = tile.x;             // ...but keep the map: rain returns
      g_keepMapY = tile.y;
      rrSweep(rrKeepCurrent);
      strlcpy(s_note, "radar quiet", sizeof(s_note));
      s_nextCycleMs = now + RR_CYCLE_MS;
      return;
    }
    // Rain. Persist the gate frame so the loop below can skip it.
    File w = LittleFS.open(gp, "w");
    if (!w || w.write(grid, RR_GRID_BYTES) != RR_GRID_BYTES) {
      if (w) w.close();
      strlcpy(s_note, "radar: fs grid write failed", sizeof(s_note));
      return;
    }
    w.close();
  }

  // 3. The map, cached on flash until the tile changes.
  {
    char mp[28];
    snprintf(mp, sizeof(mp), RR_MAP_FMT, tile.x, tile.y);
    if (!LittleFS.exists(mp)) {
      if (!rrBuildMap(tile.x, tile.y, err, sizeof(err))) {
        snprintf(s_note, sizeof(s_note), "radar map: %.38s", err);
        return;
      }
    } else {
      s_mapTileX = tile.x;
      s_mapTileY = tile.y;
    }
  }

  // 4. The rest of the hour — but only the frames flash does not already
  // hold, which in steady rain is one new frame and the nowcast.
  for (uint8_t i = 0; i < total; i++) {
    char gp[24];
    snprintf(gp, sizeof(gp), RR_GRID_FMT, (unsigned)tss[i]);
    if (LittleFS.exists(gp)) continue;
    if (!rrBuildFrame(host.c_str(), paths[i].c_str(), tile.x, tile.y,
                      tether, tss[i], grid, err, sizeof(err))) {
      snprintf(s_note, sizeof(s_note), "radar f%u: %.38s", i, err);
      return;
    }
  }

  // 5. Sweep strangers, then publish.
  g_keepN = total;
  for (uint8_t i = 0; i < total; i++) g_keepTs[i] = tss[i];
  g_keepMapX = tile.x;
  g_keepMapY = tile.y;
  rrSweep(rrKeepCurrent);

  lockTake();
  s_frames = total;
  s_nowIdx = nowIdx;
  for (uint8_t i = 0; i < total; i++) {
    s_ts[i] = tss[i];
    s_minOff[i] = offs[i];
  }
  s_markerX = tile.px;
  s_markerY = tile.py;
  s_ready = true;
  lockGive();
  snprintf(s_note, sizeof(s_note), "radar: %u frames", total);
  s_nextCycleMs = now + RR_CYCLE_MS;
}

#endif  // WITH_WEATHER
