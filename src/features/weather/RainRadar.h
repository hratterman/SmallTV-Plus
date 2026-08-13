// RainRadar.h — the arithmetic of the rain-radar timelapse, and none of its I/O.
//
// Everything here is pure: slippy-map tile math, the PNG scanline defilter,
// RainViewer's dBZ byte -> intensity nibble mapping, the 64x64 grid the frames
// are stored in, and the colours the overlay is painted with. The fetching,
// inflating and drawing live elsewhere; this header is shared with the host
// selftest, which pins the numbers the device then trusts.
//
// Measured facts this encoding rests on (tools/rainradar_selftest re-checks
// the derived values): RainViewer colour scheme 0 ("black and white") tiles
// are 256x256 8-bit RGBA PNGs where the gray value encodes reflectivity as
// gray = (dBZ + 32) * 2, and weak echo fades out through the alpha channel.
#pragma once
#include <stdint.h>
#include <math.h>

// The world, at one tile. Zoom 7 makes a 256 px tile span roughly 300 km at
// mid latitudes: a regional look — the storm two counties over is on screen,
// the storm two states over is not.
#define RR_ZOOM       7
#define RR_TILE_PX    256

// Storage. The map keeps half tile resolution (pixel-doubled back on draw);
// radar keeps a 64x64 grid of intensity nibbles — 4x4 tile pixels per cell,
// max-pooled so a small strong cell cannot average itself invisible.
#define RR_MAP_PX     128
#define RR_GRID       64
#define RR_GRID_BYTES (RR_GRID * RR_GRID / 2)

// Frames: up to an hour of history at RainViewer's 10-minute step, plus the
// nowcast (0..3 frames, 10 minutes apart) when the service is publishing one.
#define RR_PAST_FRAMES 7
#define RR_CAST_FRAMES 3
#define RR_FRAMES_MAX  (RR_PAST_FRAMES + RR_CAST_FRAMES)

// The screen shows tile pixels [8, 248): the 240x240 centre of the tile.
#define RR_CROP       8

// ---- tile addressing --------------------------------------------------------
struct RRTile {
  int     x, y;       // slippy tile indices at RR_ZOOM
  uint8_t px, py;     // the location's pixel inside that tile
};

static inline RRTile rrTileForLatLon(float lat, float lon, int z) {
  const float n = (float)(1 << z);
  if (lat > 85.05f)  lat = 85.05f;      // mercator's edge, not the planet's
  if (lat < -85.05f) lat = -85.05f;
  float xf = (lon + 180.0f) / 360.0f * n;
  const float latr = lat * (float)M_PI / 180.0f;
  float yf = (1.0f - logf(tanf(latr) + 1.0f / cosf(latr)) / (float)M_PI) / 2.0f * n;
  if (xf < 0) xf = 0;
  if (yf < 0) yf = 0;
  if (xf >= n) xf = n - 0.001f;
  if (yf >= n) yf = n - 0.001f;
  RRTile t;
  t.x = (int)xf;
  t.y = (int)yf;
  int px = (int)((xf - (float)t.x) * (float)RR_TILE_PX);
  int py = (int)((yf - (float)t.y) * (float)RR_TILE_PX);
  t.px = (uint8_t)(px > 255 ? 255 : px);
  t.py = (uint8_t)(py > 255 ? 255 : py);
  return t;
}

// ---- reflectivity -> intensity nibble --------------------------------------
// One nibble per ~6 dBZ starting where real precipitation starts: 1 at 12 dBZ
// (drizzle), 4 at 30 (rain), 7 at 48 (storm), 10+ severe. Weak echo that the
// service itself fades out (low alpha) is not weather; neither is the clear-air
// noise it ships fully opaque at gray 0..87.
static inline uint8_t rrNibble(uint8_t gray, uint8_t alpha) {
  if (alpha < 40 || gray < 88) return 0;
  const int n = (gray - 76) / 12;
  return (uint8_t)(n > 15 ? 15 : n);
}

// A cell with real rain, for the "is there anything to show" gate: nibble 2 is
// ~18 dBZ, which filters the drizzle-noise fringe a live radar always carries.
#define RR_GATE_NIBBLE 2
#define RR_GATE_CELLS  3

// ---- the 64x64 nibble grid --------------------------------------------------
static inline uint8_t rrGridGet(const uint8_t* g, int cx, int cy) {
  const uint8_t b = g[cy * (RR_GRID / 2) + (cx >> 1)];
  return (cx & 1) ? (uint8_t)(b >> 4) : (uint8_t)(b & 0x0F);
}

// Max-merge: a cell keeps its strongest pixel, never an average.
static inline void rrGridMax(uint8_t* g, int cx, int cy, uint8_t n) {
  uint8_t* b = &g[cy * (RR_GRID / 2) + (cx >> 1)];
  if (cx & 1) {
    if ((uint8_t)(*b >> 4) < n) *b = (uint8_t)((*b & 0x0F) | (n << 4));
  } else {
    if ((uint8_t)(*b & 0x0F) < n) *b = (uint8_t)((*b & 0xF0) | n);
  }
}

static inline int rrGridActive(const uint8_t* g) {
  int cells = 0;
  for (int cy = 0; cy < RR_GRID; cy++)
    for (int cx = 0; cx < RR_GRID; cx++)
      if (rrGridGet(g, cx, cy) >= RR_GATE_NIBBLE) cells++;
  return cells;
}

// ---- PNG scanline defilter --------------------------------------------------
// The five standard filters, applied in place. `cur` is the scanline without
// its filter byte; `prev` is the previous defiltered scanline (all zeros for
// the first row, per the spec).
static inline uint8_t rrPaeth(uint8_t a, uint8_t b, uint8_t c) {
  const int p = (int)a + (int)b - (int)c;
  const int pa = p > a ? p - a : a - p;
  const int pb = p > b ? p - b : b - p;
  const int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  return pb <= pc ? b : c;
}

static inline bool rrDefilter(uint8_t filter, uint8_t* cur, const uint8_t* prev,
                              int len, int bpp) {
  switch (filter) {
    case 0: return true;
    case 1:
      for (int i = bpp; i < len; i++) cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
      return true;
    case 2:
      for (int i = 0; i < len; i++) cur[i] = (uint8_t)(cur[i] + prev[i]);
      return true;
    case 3:
      for (int i = 0; i < len; i++) {
        const uint8_t a = i >= bpp ? cur[i - bpp] : 0;
        cur[i] = (uint8_t)(cur[i] + ((a + prev[i]) >> 1));
      }
      return true;
    case 4:
      for (int i = 0; i < len; i++) {
        const uint8_t a = i >= bpp ? cur[i - bpp] : 0;
        const uint8_t c = i >= bpp ? prev[i - bpp] : 0;
        cur[i] = (uint8_t)(cur[i] + rrPaeth(a, prev[i], c));
      }
      return true;
    default: return false;   // not a PNG we understand; drop the frame
  }
}

// ---- colours ----------------------------------------------------------------
#define RR_RGB565(r, g, b) \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// The classic radar ramp: greens into yellows into orange, red, magenta. Index
// 0 is unused (a cell that empty just shows the map).
static const uint16_t rrPalette[16] = {
    0x0000,
    RR_RGB565(12, 90, 12),   RR_RGB565(20, 140, 20), RR_RGB565(30, 190, 30),
    RR_RGB565(60, 230, 60),  RR_RGB565(200, 220, 40), RR_RGB565(255, 255, 0),
    RR_RGB565(255, 210, 0),  RR_RGB565(255, 150, 0), RR_RGB565(255, 100, 0),
    RR_RGB565(255, 40, 40),  RR_RGB565(220, 0, 0),   RR_RGB565(180, 0, 40),
    RR_RGB565(255, 0, 255),  RR_RGB565(200, 0, 255), RR_RGB565(255, 255, 255),
};

// 50/50 blend of two RGB565 colours: the rain stays translucent enough that
// the map underneath goes on being a map.
static inline uint16_t rrBlend565(uint16_t a, uint16_t b) {
  return (uint16_t)((((a) >> 1) & 0x7BEF) + (((b) >> 1) & 0x7BEF));
}

// The base map is stored as RGB332 at half brightness — 16 KB instead of 32,
// and dim enough that the overlay owns the scene.
static inline uint8_t rr565to332dim(uint8_t r8, uint8_t g8, uint8_t b8) {
  return (uint8_t)(((r8 >> 1) & 0xE0) | (((g8 >> 1) >> 3) & 0x1C) | ((b8 >> 1) >> 6));
}

static inline uint16_t rr332to565(uint8_t c) {
  const uint8_t r = (uint8_t)(c & 0xE0);
  const uint8_t g = (uint8_t)((c & 0x1C) << 3);
  const uint8_t b = (uint8_t)((c & 0x03) << 6);
  return RR_RGB565(r | (r >> 3), g | (g >> 3), b | (b >> 2));
}

// ---- timeline geometry ------------------------------------------------------
#define RR_TL_X0 16
#define RR_TL_W  208
#define RR_TL_Y  231

static inline int rrTimelineX(int frame, int frames) {
  if (frames <= 1) return RR_TL_X0 + RR_TL_W;
  return RR_TL_X0 + (frame * RR_TL_W) / (frames - 1);
}
