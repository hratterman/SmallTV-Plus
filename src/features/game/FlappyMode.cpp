#include "config.h"
#if WITH_GAME

#include "FlappyMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include <LittleFS.h>

// main.cpp: step to the next mode. The game holds the tap, so long-press is the
// only way out and it has to be able to ask.
extern void appNextMode();

FlappyMode g_flappyMode;

#define C_SKY    0x1CBF   // dusk blue
#define C_HUD    0x0A5B
#define C_PIPE   0x3E68   // pipe green
#define C_LIP    0x2C05   // darker band at the gap edge
#define C_GROUND 0x9B65
#define C_DIRT   0x6B00
#define C_BIRD   0xFE60   // yellow
#define C_BEAK   0xFB00
#define C_DIM    0xB574

// Layout. The playfield is what the pipes and the bird share; the header keeps
// the score out of their way and the ground gives the fall somewhere to land.
#define HUD_H     24
#define GROUND_H  22
#define PLAY_TOP  HUD_H
#define PLAY_BOT  (TFT_HEIGHT - GROUND_H)
#define PLAY_H    (PLAY_BOT - PLAY_TOP)

#define BIRD_X    64
#define BIRD_W    15
#define BIRD_H    12

#define PIPE_W    30
#define PIPE_STEP 108           // horizontal spacing between pipes
#define LIP_H     5
#define GAP_MAX   66
#define GAP_MIN   50

#define SCROLL_DX 2             // pixels per frame; 30 fps => 60 px/s
#define FRAME_MS  33
#define GRAVITY   0.42f
#define FLAP_V    (-4.9f)
#define MAX_FALL  7.5f

#define BEST_PATH "/flappy.dat"

static inline uint32_t xr(uint32_t* s) {
  uint32_t v = *s;
  v ^= v << 13; v ^= v >> 17; v ^= v << 5;
  return *s = v;
}

// The best score is four bytes of its own rather than a settings field: it
// changes at a different rhythm to everything else, and rewriting the whole
// config.json to record a game would be a strange thing for a game to do.
static uint16_t bestLoad() {
  File f = LittleFS.open(BEST_PATH, "r");
  if (!f) return 0;
  uint16_t v = 0;
  f.read((uint8_t*)&v, sizeof(v));
  f.close();
  return v;
}

static void bestSave(uint16_t v) {
  File f = LittleFS.open(BEST_PATH, "w");
  if (!f) return;
  f.write((const uint8_t*)&v, sizeof(v));
  f.close();
}

// ---------------------------------------------------------------------------
void FlappyMode::begin(const Settings& s) {
  best_ = bestLoad();
  invalidate(s);
}

void FlappyMode::invalidate(const Settings& s) {
  reset();
  needFull_ = true;
}

void FlappyMode::wake(const Settings& s) {
  // Arriving back on the screen never resumes a run: the bird would be mid-air
  // with no warning. Reset to the ready prompt instead.
  reset();
  needFull_ = true;
}

void FlappyMode::onContextAction(Settings& s) { appNextMode(); }

void FlappyMode::reset() {
  state_ = READY;
  birdY_ = PLAY_TOP + PLAY_H / 2.0f - BIRD_H / 2.0f;
  vel_ = 0;
  drawnY_ = -1;
  score_ = 0;
  drawnScore_ = drawnBest_ = 0xFFFF;
  rng_ = (uint32_t)millis() | 1u;
  for (int i = 0; i < FLAP_PIPES; i++) pipes_[i].active = false;
}

void FlappyMode::onTap(Settings& s) {
  if (state_ == READY) {
    state_ = PLAYING;
    vel_ = FLAP_V;
    // First pipe starts off the right edge so there is a moment to react.
    for (int i = 0; i < FLAP_PIPES; i++)
      spawnPipe(i, TFT_WIDTH + 40 + i * PIPE_STEP);
    lastFrame_ = millis();
  } else if (state_ == PLAYING) {
    vel_ = FLAP_V;
    wing_ = 6;                       // frames of "wing up" after a flap
  } else if (state_ == DEAD) {
    // Brief lockout so the tap that killed you does not also restart you.
    if (millis() - deadAt_ > 600) { reset(); needFull_ = true; }
  }
}

// ---------------------------------------------------------------------------
void FlappyMode::spawnPipe(int slot, int16_t atX) {
  Pipe& p = pipes_[slot];
  // The gap tightens with the score and then stops, so the run gets harder
  // without ever becoming impossible.
  int gap = GAP_MAX - (int)score_;
  if (gap < GAP_MIN) gap = GAP_MIN;
  const int margin = 18;
  const int span = PLAY_H - gap - margin * 2;
  p.gapY = (int16_t)(PLAY_TOP + margin + (span > 0 ? (int)(xr(&rng_) % span) : 0));
  p.x = atX;
  p.scored = false;
  p.active = true;
}

static inline int pipeGap(uint16_t score) {
  int gap = GAP_MAX - (int)score;
  return gap < GAP_MIN ? GAP_MIN : gap;
}

// ---------------------------------------------------------------------------
void FlappyMode::drawChrome() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  gfx->fillRect(0, 0, TFT_WIDTH, HUD_H, C_HUD);
  gfx->fillRect(0, PLAY_TOP, TFT_WIDTH, PLAY_H, C_SKY);
  gfx->fillRect(0, PLAY_BOT, TFT_WIDTH, GROUND_H, C_GROUND);
  gfx->fillRect(0, PLAY_BOT, TFT_WIDTH, 3, C_DIRT);
  drawScore(true);
}

void FlappyMode::drawScore(bool force) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  char b[16];
  if (force || score_ != drawnScore_) {
    drawnScore_ = score_;
    snprintf(b, sizeof(b), "%u", (unsigned)score_);
    gfx->fillRect(8, 4, 70, 16, C_HUD);
    gfx->setTextSize(2);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(8, 5);
    gfx->print(b);
  }
  if (force || best_ != drawnBest_) {
    drawnBest_ = best_;
    snprintf(b, sizeof(b), "best %u", (unsigned)best_);
    const int w = gfxTextW(b, 1);
    gfx->fillRect(TFT_WIDTH - w - 10, 4, w + 8, 12, C_HUD);
    gfx->setTextSize(1);
    gfx->setTextColor(C_DIM);
    gfx->setCursor(TFT_WIDTH - w - 8, 9);
    gfx->print(b);
  }
}

// Repaint whatever pipe covers this rectangle. Used after the bird's old box is
// wiped to sky, so a pipe it was flying past does not end up with a hole in it.
void FlappyMode::patchPipes(int16_t x, int16_t y, int16_t w, int16_t h) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const int gap = pipeGap(score_);
  for (int i = 0; i < FLAP_PIPES; i++) {
    const Pipe& p = pipes_[i];
    if (!p.active) continue;
    const int x0 = p.x > x ? p.x : x;
    const int x1 = (p.x + PIPE_W) < (x + w) ? (p.x + PIPE_W) : (x + w);
    if (x1 <= x0) continue;

    const int spans[2][2] = {{PLAY_TOP, p.gapY}, {p.gapY + gap, PLAY_BOT}};
    for (int sIdx = 0; sIdx < 2; sIdx++) {
      const int y0 = spans[sIdx][0] > y ? spans[sIdx][0] : y;
      const int y1 = spans[sIdx][1] < (y + h) ? spans[sIdx][1] : (y + h);
      if (y1 <= y0) continue;
      gfx->fillRect(x0, y0, x1 - x0, y1 - y0, C_PIPE);
    }
    // The dark band at each gap edge, where it falls inside the patch.
    const int lips[2] = {p.gapY - LIP_H, p.gapY + gap};
    for (int l = 0; l < 2; l++) {
      const int y0 = lips[l] > y ? lips[l] : y;
      const int y1 = (lips[l] + LIP_H) < (y + h) ? (lips[l] + LIP_H) : (y + h);
      if (y1 > y0) gfx->fillRect(x0, y0, x1 - x0, y1 - y0, C_LIP);
    }
  }
}

// Scrolling without repainting: each pipe gives back a strip of sky on its
// right and claims a strip of pipe on its left. Everything between is already
// correct from previous frames.
void FlappyMode::scrollPipes(int dx) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const int gap = pipeGap(score_);

  for (int i = 0; i < FLAP_PIPES; i++) {
    Pipe& p = pipes_[i];
    if (!p.active) continue;

    // Sky where the trailing edge was. Clipped, because a pipe leaving the
    // screen has its right edge off the left side.
    int vx = p.x + PIPE_W - dx;
    int vw = dx;
    if (vx < 0) { vw += vx; vx = 0; }
    if (vx + vw > TFT_WIDTH) vw = TFT_WIDTH - vx;
    if (vw > 0) gfx->fillRect(vx, PLAY_TOP, vw, PLAY_H, C_SKY);

    p.x -= dx;

    // Pipe where the leading edge now is.
    int lx = p.x, lw = dx;
    if (lx < 0) { lw += lx; lx = 0; }
    if (lx + lw > TFT_WIDTH) lw = TFT_WIDTH - lx;
    if (lw > 0 && p.x + PIPE_W > 0 && p.x < TFT_WIDTH) {
      gfx->fillRect(lx, PLAY_TOP, lw, p.gapY - PLAY_TOP, C_PIPE);
      gfx->fillRect(lx, p.gapY + gap, lw, PLAY_BOT - (p.gapY + gap), C_PIPE);
      gfx->fillRect(lx, p.gapY - LIP_H, lw, LIP_H, C_LIP);
      gfx->fillRect(lx, p.gapY + gap, lw, LIP_H, C_LIP);
    }

    if (p.x + PIPE_W <= 0) {
      // Recycle to the far side, one full spacing behind the rightmost pipe.
      int16_t furthest = 0;
      for (int j = 0; j < FLAP_PIPES; j++)
        if (pipes_[j].active && pipes_[j].x > furthest) furthest = pipes_[j].x;
      spawnPipe(i, furthest + PIPE_STEP);
    } else if (!p.scored && p.x + PIPE_W < BIRD_X) {
      p.scored = true;
      score_++;
    }
  }
}

void FlappyMode::eraseBird() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx || drawnY_ < 0) return;
  gfx->fillRect(BIRD_X, drawnY_, BIRD_W, BIRD_H, C_SKY);
  patchPipes(BIRD_X, drawnY_, BIRD_W, BIRD_H);
}

void FlappyMode::drawBird() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const int y = (int)birdY_;
  gfx->fillRoundRect(BIRD_X, y, BIRD_W - 3, BIRD_H, 4, C_BIRD);
  // Wing rides high just after a flap and settles as the bird falls: a cheap
  // way to show what the tap did without rotating a sprite.
  const int wy = y + (wing_ ? 2 : 6);
  gfx->fillRect(BIRD_X + 2, wy, 6, 3, C_WHITE);
  gfx->fillRect(BIRD_X + 8, y + 3, 3, 3, C_WHITE);     // eye white
  gfx->fillRect(BIRD_X + 9, y + 4, 2, 2, C_BLACK);     // pupil
  gfx->fillRect(BIRD_X + BIRD_W - 4, y + 6, 4, 3, C_BEAK);
  drawnY_ = (int16_t)y;
}

bool FlappyMode::collides() const {
  const int y = (int)birdY_;
  if (y + BIRD_H >= PLAY_BOT) return true;
  if (y <= PLAY_TOP) return true;
  const int gap = pipeGap(score_);
  for (int i = 0; i < FLAP_PIPES; i++) {
    const Pipe& p = pipes_[i];
    if (!p.active) continue;
    if (BIRD_X + BIRD_W <= p.x || BIRD_X >= p.x + PIPE_W) continue;
    if (y < p.gapY || y + BIRD_H > p.gapY + gap) return true;
  }
  return false;
}

void FlappyMode::drawBanner() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const int by = PLAY_TOP + PLAY_H / 2 - 30;
  gfx->fillRoundRect(24, by, TFT_WIDTH - 48, 62, 8, C_HUD);
  if (state_ == READY) {
    gfxDrawCentered("TAP TO FLAP", by + 14, 2, C_WHITE);
    gfxDrawCentered("hold to leave", by + 40, 1, C_DIM);
  } else {
    char b[24];
    snprintf(b, sizeof(b), "SCORE %u", (unsigned)score_);
    gfxDrawCentered(b, by + 12, 2, C_WHITE);
    gfxDrawCentered(score_ >= best_ && score_ ? "new best!" : "tap to retry",
                    by + 40, 1, score_ >= best_ && score_ ? C_BIRD : C_DIM);
  }
}

// ---------------------------------------------------------------------------
void FlappyMode::service(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  if (needFull_) {
    needFull_ = false;
    drawChrome();
    drawBird();
    if (state_ != PLAYING) drawBanner();
    return;
  }
  if (state_ != PLAYING) return;      // nothing moves until a tap starts it

  const uint32_t now = millis();
  if (now - lastFrame_ < FRAME_MS) return;
  lastFrame_ = now;

  eraseBird();
  scrollPipes(SCROLL_DX);

  vel_ += GRAVITY;
  if (vel_ > MAX_FALL) vel_ = MAX_FALL;
  birdY_ += vel_;
  if (birdY_ < PLAY_TOP) { birdY_ = PLAY_TOP; vel_ = 0; }
  if (wing_) wing_--;

  if (collides()) {
    if (birdY_ + BIRD_H > PLAY_BOT) birdY_ = PLAY_BOT - BIRD_H;
    drawBird();
    state_ = DEAD;
    deadAt_ = now;
    if (score_ > best_) { best_ = score_; bestSave(best_); }
    drawScore(false);
    drawBanner();
    return;
  }

  drawBird();
  drawScore(false);
}

#endif  // WITH_GAME
