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

class AmbientMode : public DisplayMode {
 public:
  const char* id() const override { return "ambient"; }
  uint8_t     modeConst() const override { return MODE_AMBIENT; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override { needFull_ = true; }
  void onContextAction(Settings& s) override;   // long-press: next pattern

 private:
  void startPattern(const Settings& s);
  void stepLife();
  void stepPlasma();
  void stepStars();

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

  uint16_t phase_ = 0;
};

extern AmbientMode g_ambientMode;

#endif  // WITH_AMBIENT
