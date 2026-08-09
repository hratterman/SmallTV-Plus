// AmbientMode.h — the screen with nothing to tell you.
//
// Every other mode is information. This one is the cube just being a nice
// object on a desk: no network, no fetching, nothing that can go stale or
// error. Long-press cycles between the patterns.
//
// All three are written around the same constraint, which is that the SPI bus,
// not the CPU, decides the frame rate. Repainting 240x240 costs 115 KB over the
// wire, so nothing here ever repaints the screen. Life and the starfield touch
// only the cells that changed; the plasma pushes one row at a time from a line
// buffer and runs at half resolution, which is invisible on a pattern with no
// hard edges and quarters the arithmetic.
#pragma once
#include "config.h"
#if WITH_AMBIENT

#include "Mode.h"

// Cell grid for Life. 60x60 at 4 px fills the panel exactly.
#define AMB_CELLS 60
#define AMB_CELL_PX (TFT_WIDTH / AMB_CELLS)
#define AMB_STARS 48
#define AMB_COLS  40      // matrix rain columns, 6 px apart
#define AMB_SPARKS 96     // firework particles alive at once (two bursts' worth)
#define AMB_BURST  32     // particles per shell — enough that the ring reads as one
#define AMB_SPARK_PX 2    // particle size; one pixel is a speck, not a spark

class AmbientMode : public DisplayMode {
 public:
  const char* id() const override { return "ambient"; }
  uint8_t     modeConst() const override { return MODE_AMBIENT; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override { needFull_ = true; }
  void onContextAction(Settings& s) override;   // long-press: next pattern

  // Ambient is meant to be left running, so it holds the carousel for minutes
  // rather than the glanceable dwell the informational screens use.
  uint16_t dwellSec(const Settings& s) const override;

 private:
  void startPattern(const Settings& s);
  void stepLife();
  void stepPlasma();
  void stepStars();
  void stepRain();
  void stepSparks();
  void nextPattern(const Settings& s);

  uint8_t  pattern_ = 0;
  bool     needFull_ = true;
  uint32_t lastMs_ = 0;
  uint32_t rng_ = 22695477u;
  uint16_t gen_ = 0;

  // Life. Two grids, one byte per cell: bitpacking would save 6 KB but makes
  // the neighbour count the expensive part, and the heap has room.
  uint8_t  cells_[AMB_CELLS][AMB_CELLS] = {};
  uint8_t  next_[AMB_CELLS][AMB_CELLS] = {};

  struct Star { int16_t x, y; uint8_t z, px, py; bool drawn; };
  Star     stars_[AMB_STARS] = {};

  // Matrix rain: one head position and speed per column, and the tail is drawn
  // by simply not erasing behind it until the head wraps.
  int16_t  rainY_[AMB_COLS] = {};
  uint8_t  rainSpd_[AMB_COLS] = {};

  // Position and velocity are 8.8 fixed point. Integer pixels-per-frame made
  // the slowest non-zero particle cross 30 px/s and the fastest 210, which is
  // why the old bursts looked like a handful of darting specks: there was no
  // room between "barely moves" and "gone in three frames".
  struct Spark {
    int16_t  x, y;         // 8.8 position
    int16_t  vx, vy;       // 8.8 velocity, px per frame
    int16_t  px, py;       // last drawn pixel (whole pixels), -1 = not drawn
    uint8_t  life, age;    // frames remaining, frames lived
    uint16_t col;
  };
  Spark    sparks_[AMB_SPARKS] = {};
  uint16_t sparkTimer_ = 0;

  uint32_t patternSince_ = 0;
  uint16_t phase_ = 0;
};

extern AmbientMode g_ambientMode;

#endif  // WITH_AMBIENT
