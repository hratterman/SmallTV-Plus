// Self-test for RainRadar.h — the pure arithmetic under the radar timelapse.
//
// The values pinned here were measured, not assumed: the Tampa tile address
// against the slippy-map formula's reference implementation, and the nibble
// mapping against pixels pulled out of a real RainViewer colour-scheme-0 tile
// during a Florida thunderstorm (gray 130 / alpha 73 inside the storm, gray 0 /
// alpha 255 in the clear-air ring around it).
#include <cstdio>
#include <cstring>
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

  printf(fails ? "\n%d FAILURES\n" : "\nall ok\n", fails);
  return fails ? 1 : 0;
}
