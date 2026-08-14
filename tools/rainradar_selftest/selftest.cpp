// Self-test for RainRadar.h — the pure arithmetic under the radar timelapse.
//
// The values pinned here were measured, not assumed: the Tampa tile address
// against the slippy-map formula's reference implementation, and the nibble
// mapping against pixels pulled out of a real RainViewer colour-scheme-0 tile
// during a Florida thunderstorm (gray 130 / alpha 73 inside the storm, gray 0 /
// alpha 255 in the clear-air ring around it).
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <zlib.h>
extern "C" {
#include "../../src/vendor/uzlib/uzlib.h"
}
#include "../../src/features/weather/RainRadar.h"

static int fails = 0;
#define CHECK(cond, name)                                   \
  do {                                                      \
    if (cond) { printf("ok   %s\n", name); }                \
    else      { printf("FAIL %s\n", name); fails++; }       \
  } while (0)

int main() {
  // ---- tile math ----------------------------------------------------------
  {
    // Tampa, FL. Reference: x = (lon+180)/360*128 = 34.68, mercator y = 53.65.
    RRTile t = rrTileForLatLon(27.95f, -82.46f, 7);
    CHECK(t.x == 34 && t.y == 53, "tile: Tampa lands on 7/34/53");
    CHECK(t.px > 165 && t.px < 185, "tile: Tampa x-pixel near 174");
    CHECK(t.py > 155 && t.py < 175, "tile: Tampa y-pixel near 166");
  }
  {
    RRTile t = rrTileForLatLon(51.5074f, -0.1278f, 7);   // London
    CHECK(t.x == 63 && t.y == 42, "tile: London lands on 7/63/42");
  }
  {
    RRTile t = rrTileForLatLon(89.0f, 179.9f, 7);        // near the pole: clamped
    CHECK(t.y == 0 && t.x == 127, "tile: polar latitude clamps, not overflows");
  }

  // ---- dBZ nibble mapping -------------------------------------------------
  CHECK(rrNibble(0, 255) == 0,   "nibble: opaque clear-air gray 0 is nothing");
  CHECK(rrNibble(87, 255) == 0,  "nibble: below 12 dBZ is nothing");
  CHECK(rrNibble(88, 255) == 1,  "nibble: 12 dBZ starts at 1");
  CHECK(rrNibble(130, 73) == 4,  "nibble: measured storm pixel (33 dBZ) -> 4");
  CHECK(rrNibble(130, 20) == 0,  "nibble: faded-out echo (low alpha) ignored");
  CHECK(rrNibble(196, 255) == 10, "nibble: 66 dBZ severe -> 10");
  CHECK(rrNibble(255, 255) == 14, "nibble: the brightest byte stays in range");

  // ---- grid pack / max-merge ---------------------------------------------
  {
    uint8_t g[RR_GRID_BYTES];
    memset(g, 0, sizeof(g));
    rrGridMax(g, 5, 9, 7);
    rrGridMax(g, 5, 9, 3);                 // weaker: must not overwrite
    rrGridMax(g, 4, 9, 12);                // the even-index neighbour nibble
    CHECK(rrGridGet(g, 5, 9) == 7,  "grid: max-merge keeps the strongest");
    CHECK(rrGridGet(g, 4, 9) == 12, "grid: neighbour nibble is independent");
    CHECK(rrGridGet(g, 6, 9) == 0,  "grid: untouched cell stays empty");
    CHECK(rrGridActive(g) == 2,     "grid: two active cells counted");
    rrGridMax(g, 0, 0, 1);                 // drizzle: below the gate
    CHECK(rrGridActive(g) == 2,     "grid: nibble 1 does not count as weather");
  }

  // ---- PNG defilter -------------------------------------------------------
  {
    // Sub (1): cumulative along the row, bpp apart.
    uint8_t cur[8]  = {10, 20, 5, 5, 1, 1, 1, 1};
    uint8_t prev[8] = {0};
    rrDefilter(1, cur, prev, 8, 4);
    CHECK(cur[4] == 11 && cur[5] == 21 && cur[6] == 6 && cur[7] == 6,
          "defilter: sub adds the byte bpp back");
  }
  {
    // Up (2): cumulative down the column.
    uint8_t cur[4]  = {5, 5, 5, 5};
    uint8_t prev[4] = {100, 200, 250, 3};
    rrDefilter(2, cur, prev, 4, 4);
    CHECK(cur[0] == 105 && cur[1] == 205 && cur[2] == 255 && cur[3] == 8,
          "defilter: up adds the row above, mod 256");
  }
  {
    // Average (3): floor((a+b)/2).
    uint8_t cur[8]  = {10, 10, 10, 10, 10, 10, 10, 10};
    uint8_t prev[8] = {20, 20, 20, 20, 20, 20, 20, 20};
    rrDefilter(3, cur, prev, 8, 4);
    CHECK(cur[0] == 20 && cur[4] == 30,
          "defilter: average uses zero left of the first pixel");
  }
  {
    // Paeth (4): the predictor picks the neighbour closest to a+b-c.
    CHECK(rrPaeth(10, 10, 30) == 10, "paeth: an a/b tie goes left");
    CHECK(rrPaeth(100, 20, 15) == 100, "paeth: p=105 prefers a=100");
    CHECK(rrPaeth(10, 90, 100) == 10, "paeth: c above both pulls left");
    uint8_t cur[8]  = {1, 1, 1, 1, 2, 2, 2, 2};
    uint8_t prev[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    rrDefilter(4, cur, prev, 8, 4);
    CHECK(cur[0] == 10 && cur[4] == 12, "defilter: paeth row reconstructs");
    CHECK(!rrDefilter(9, cur, prev, 8, 4), "defilter: unknown filter refused");
  }

  // ---- colours ------------------------------------------------------------
  CHECK(rrPalette[0] == 0, "palette: empty cell has no colour of its own");
  CHECK(rrPalette[15] == 0xFFFF, "palette: the top of the ramp is white");
  CHECK(rrBlend565(0xFFFF, 0x0000) == 0x7BEF, "blend: half white is mid gray");
  {
    const uint8_t c = rr565to332dim(255, 255, 255);
    const uint16_t back = rr332to565(c);
    CHECK(c == 0x6D, "map store: white dims to half in RGB332");
    // Half brightness expanded back: every channel near its 50% mark.
    CHECK(((back >> 11) & 0x1F) >= 12 && ((back >> 11) & 0x1F) <= 16,
          "map store: round-trip red lands near half");
  }

  // ---- timeline -----------------------------------------------------------
  CHECK(rrTimelineX(0, 10) == RR_TL_X0, "timeline: first frame at the left edge");
  CHECK(rrTimelineX(9, 10) == RR_TL_X0 + RR_TL_W, "timeline: last frame at the right");
  CHECK(rrTimelineX(1, 2) == RR_TL_X0 + RR_TL_W, "timeline: two frames span it all");

  // ---- the real tile, end to end ------------------------------------------
  // fixture_storm.png is an actual RainViewer colour-scheme-0 tile pulled over
  // a Florida thunderstorm (z7 x34 y53). The device's own scanline assembler
  // (rrScanConsume, shared via RainRadar.h) is fed the zlib-inflated stream in
  // deliberately awkward 997-byte pieces — the sizes the ROM inflater hands
  // out are just as arbitrary — and must land on the count an independent
  // Python decode of the same tile produced: 490 cells of real rain.
  {
    const char* fx = getenv("RR_FIXTURE");
    FILE* f = fopen(fx ? fx : "fixture_storm.png", "rb");
    CHECK(f != nullptr, "fixture: storm tile present");
    if (f) {
      std::vector<uint8_t> png;
      uint8_t rb[4096];
      size_t got;
      while ((got = fread(rb, 1, sizeof(rb), f)) > 0) png.insert(png.end(), rb, rb + got);
      fclose(f);

      // Concatenate the IDAT payloads, exactly as the device's chunk walk does.
      std::vector<uint8_t> idat;
      for (size_t off = 8; off + 12 <= png.size();) {
        const uint32_t clen = ((uint32_t)png[off] << 24) | ((uint32_t)png[off + 1] << 16) |
                              ((uint32_t)png[off + 2] << 8) | png[off + 3];
        if (!memcmp(&png[off + 4], "IDAT", 4))
          idat.insert(idat.end(), &png[off + 8], &png[off + 8] + clen);
        if (!memcmp(&png[off + 4], "IEND", 4)) break;
        off += 12 + clen;
      }
      CHECK(!idat.empty(), "fixture: IDAT found");

      std::vector<uint8_t> raw(257 * 1025);   // decompressed stream, with slack
      uLongf rawLen = raw.size();
      const int zr = uncompress(raw.data(), &rawLen, idat.data(), idat.size());
      CHECK(zr == Z_OK && rawLen == 256 * 1025, "fixture: zlib stream inflates whole");

      uint8_t cur[1 + RR_TILE_PX * 4], prev[RR_TILE_PX * 4];
      uint8_t grid[RR_GRID_BYTES];
      memset(prev, 0, sizeof(prev));
      memset(grid, 0, sizeof(grid));
      RRScan scan = {cur, prev, grid, 0, 0, false};
      for (size_t p = 0; p < rawLen; p += 997)
        rrScanConsume(scan, raw.data() + p, rawLen - p < 997 ? rawLen - p : 997);

      CHECK(!scan.bad, "real tile: every filter understood");
      CHECK(scan.y == RR_TILE_PX, "real tile: all 256 scanlines assembled");
      const int active = rrGridActive(grid);
      printf("     real tile: %d active cells\n", active);
      CHECK(active == 490, "real tile: matches the Python reference (490)");
      CHECK(active >= RR_GATE_CELLS, "real tile: a storm passes the gate");

      // The same tile again, through the inflater the DEVICE actually ships —
      // the vendored uzlib — fed via the read callback in 512-byte refills,
      // output drained in 1 KB chunks, dictionary ring in play. This is the
      // exact engine and calling pattern of RainRadarClient's decode.
      {
        struct Src {
          struct uzlib_uncomp u;             // first member: cb containerofs it
          const uint8_t* p;
          size_t left;
          uint8_t in[512];
        } s;
        memset(&s.u, 0, sizeof(s.u));
        s.p = idat.data();
        s.left = idat.size();
        s.u.source = s.u.source_limit = nullptr;
        s.u.source_read_cb = [](struct uzlib_uncomp* u) -> int {
          Src* c = (Src*)u;
          if (!c->left) return -1;
          size_t take = c->left < sizeof(c->in) ? c->left : sizeof(c->in);
          memcpy(c->in, c->p, take);
          c->p += take;
          c->left -= take;
          u->source = c->in + 1;
          u->source_limit = c->in + take;
          return c->in[0];
        };
        std::vector<uint8_t> dict(32768);
        uzlib_uncompress_init(&s.u, dict.data(), 32768);
        CHECK(uzlib_zlib_parse_header(&s.u) >= 0, "uzlib: zlib header accepted");

        uint8_t cur2[1 + RR_TILE_PX * 4], prev2[RR_TILE_PX * 4];
        uint8_t grid2[RR_GRID_BYTES];
        memset(prev2, 0, sizeof(prev2));
        memset(grid2, 0, sizeof(grid2));
        RRScan scan2 = {cur2, prev2, grid2, 0, 0, false};
        uint8_t out[1024];
        bool done = false, corrupt = false;
        while (!done && !corrupt && !scan2.bad) {
          s.u.dest_start = s.u.dest = out;
          s.u.dest_limit = out + sizeof(out);
          const int res = uzlib_uncompress(&s.u);
          const size_t got = (size_t)(s.u.dest - out);
          if (got) rrScanConsume(scan2, out, got);
          if (res == TINF_DONE) done = true;
          else if (res != TINF_OK) corrupt = true;
          else if (!got) corrupt = true;
        }
        CHECK(done && !corrupt, "uzlib: the stream inflates to DONE");
        CHECK(scan2.y == RR_TILE_PX, "uzlib: all 256 scanlines arrive");
        CHECK(rrGridActive(grid2) == 490,
              "uzlib: the device engine matches the reference (490)");
      }
    }
  }

  printf(fails ? "\n%d FAILURES\n" : "\nall ok\n", fails);
  return fails ? 1 : 0;
}
