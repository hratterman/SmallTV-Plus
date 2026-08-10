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

  // The pattern buffers are ~9.5 KB and every byte of them starts at zero. They
  // are deliberately NOT members: this class is polymorphic, so its vtable
  // pointer is a non-zero initialiser, which puts the whole object in .data —
  // and the flash image then carries a literal copy of 9.5 KB of zeroes. At
  // file scope in the .cpp they are plain zero-initialised statics, land in
  // .bss, and cost the same RAM for none of the flash. There is exactly one
  // AmbientMode, so nothing is lost by not scoping them to the instance.
  uint8_t  pattern_ = 0;
  bool     needFull_ = true;
  uint32_t lastMs_ = 0;
  uint32_t rng_ = 22695477u;
  uint16_t gen_ = 0;
  uint16_t sparkTimer_ = 0;
  uint32_t patternSince_ = 0;
  uint16_t phase_ = 0;
};

extern AmbientMode g_ambientMode;

#endif  // WITH_AMBIENT
