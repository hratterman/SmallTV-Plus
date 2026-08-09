// FlappyMode.h — the one game a single touch pad can actually carry.
//
// Tap to flap, long-press to leave. The pad is the whole control surface, which
// rules out anything needing aim or two simultaneous inputs, and leaves timing
// games: this one, because it explains itself in one attempt.
//
// The design constraint is the SPI bus, not the CPU. A full 240x240 repaint is
// 115 KB over the wire and would cap the frame rate around 15 fps on its own,
// so nothing here repaints. Pipes scroll by drawing a two-pixel strip at the
// leading edge and a two-pixel strip of sky where the trailing edge was; a pipe
// costs the same per frame whether it is on screen for one second or four. The
// bird erases its own box and any pipe it was covering gets patched back.
//
// The score lives in a header band the pipes stop below. Floating it over the
// playfield would mean repainting text under a moving pipe every frame, and
// erase-then-draw at 30 fps is visible as flicker.
#pragma once
#include "config.h"
#if WITH_GAME

#include "Mode.h"

#define FLAP_PIPES 3

class FlappyMode : public DisplayMode {
 public:
  const char* id() const override { return "flappy"; }
  uint8_t     modeConst() const override { return MODE_FLAPPY; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings& s) override;
  void onContextAction(Settings& s) override;   // long-press: back to the modes

  bool wantsTap() const override { return true; }
  void onTap(Settings& s) override;
  bool holdsScreen() const override { return state_ == PLAYING; }

 private:
  enum State : uint8_t { READY, PLAYING, DEAD };

  void reset();
  void drawChrome();
  void drawBird();
  void eraseBird();
  void patchPipes(int16_t x, int16_t y, int16_t w, int16_t h);
  void scrollPipes(int dx);
  void spawnPipe(int slot, int16_t atX);
  void drawScore(bool force);
  void drawBanner();
  bool collides() const;

  State    state_ = READY;
  bool     needFull_ = true;
  uint32_t lastFrame_ = 0;
  uint32_t deadAt_ = 0;
  uint32_t rng_ = 12345u;

  float    birdY_ = 0, vel_ = 0;
  int16_t  drawnY_ = -1;
  uint8_t  wing_ = 0;

  struct Pipe { int16_t x; int16_t gapY; bool scored; bool active; };
  Pipe     pipes_[FLAP_PIPES] = {};

  uint16_t score_ = 0, best_ = 0, drawnScore_ = 0xFFFF, drawnBest_ = 0xFFFF;
};

extern FlappyMode g_flappyMode;

#endif  // WITH_GAME
