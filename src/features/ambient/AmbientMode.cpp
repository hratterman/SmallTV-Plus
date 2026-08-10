#include "config.h"
#if WITH_AMBIENT

#include "AmbientMode.h"
#include "AmbientPick.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Settings.h"
#include <math.h>

AmbientMode g_ambientMode;

// The pattern buffers — see the note in AmbientMode.h for why these are here
// rather than in the class. Zero-initialised statics, so .bss rather than .data.
//
// Life keeps two grids, one byte per cell: bitpacking would save 6 KB but makes
// the neighbour count the expensive part, and the RAM is there.
static uint8_t cells_[AMB_CELLS][AMB_CELLS];
static uint8_t next_[AMB_CELLS][AMB_CELLS];

struct Star { int16_t x, y; uint8_t z, px, py; bool drawn; };
static Star stars_[AMB_STARS];

// Matrix rain: one head position and speed per column; the tail is drawn by
// simply not erasing behind it until the head wraps.
static int16_t rainY_[AMB_COLS];
static uint8_t rainSpd_[AMB_COLS];

// Fireworks. Position and velocity are 8.8 fixed point — 32-bit for position
// because 240<<8 does not fit int16_t, which is what made the first version's
// shells vanish on their opening frame.
struct Spark {
  int32_t  x, y;
  int16_t  vx, vy;
  int16_t  px, py;       // last drawn pixel, -1 = nothing to erase
  uint8_t  life, age;
  uint16_t col;
};
static Spark sparks_[AMB_SPARKS];

// AMB_PAT_* and AMB_PATTERNS live in config.h — Settings.cpp needs them too.
#define PAT_LIFE   AMB_PAT_LIFE
#define PAT_PLASMA AMB_PAT_PLASMA
#define PAT_STARS  AMB_PAT_STARS
#define PAT_RAIN   AMB_PAT_RAIN
#define PAT_SPARKS AMB_PAT_SPARKS

// Pattern selection lives in AmbientPick.h so it can be run on a host.

// A cell's colour by how long it has been alive: new cells are bright, settled
// ones cool off. Turns a binary automaton into something with depth.
static const uint16_t kLifeAge[4] = {0x07FF, 0x05BF, 0x033F, 0x01DF};

static inline uint32_t xr(uint32_t* s) {
  uint32_t v = *s;
  v ^= v << 13; v ^= v >> 17; v ^= v << 5;
  return *s = v;
}

// invalidate() raises needFull_, which is what the zero-initialised members
// above rely on: nothing draws until the first startPattern() has run.
void AmbientMode::begin(const Settings& s) {
  // Don't always open on Life. Shuffle is the existing "I want variety" switch,
  // so honour it at the start of the run too rather than only between patterns.
  rng_ = ((uint32_t)millis() * 2654435761u) | 1u;
  pattern_ = ambFirst(s.ambient.patternMask, s.ambient.shuffle, xr(&rng_));
  invalidate(s);
}

// Repaint, but stay on the pattern that is running. Resetting to Life here
// meant every unrelated save — a ticker symbol, a WiFi password — yanked the
// screen back to the first pattern, since the web portal invalidates every mode.
void AmbientMode::invalidate(const Settings& s) {
  needFull_ = true;
}

uint16_t AmbientMode::dwellSec(const Settings& s) const {
  return s.ambient.dwellSec ? s.ambient.dwellSec : DEFAULT_AMBIENT_DWELL_SEC;
}

// Long-press always steps in order: when you ask for the next one you want the
// next one, not a coin flip.
void AmbientMode::onContextAction(Settings& s) {
  pattern_ = ambNextOn(s.ambient.patternMask, pattern_);
  needFull_ = true;
}

// The automatic advance inside a long block can shuffle, so a three-minute
// stretch does not always run the patterns in the same order.
void AmbientMode::nextPattern(const Settings& s) {
  pattern_ = s.ambient.shuffle
                 ? ambShuffleNext(s.ambient.patternMask, pattern_, xr(&rng_))
                 : ambNextOn(s.ambient.patternMask, pattern_);
  needFull_ = true;
}

void AmbientMode::startPattern(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  // The running pattern can have been unticked while it was on screen, and the
  // random start above does not know the mask either.
  if (!ambPatternOn(s.ambient.patternMask, pattern_))
    pattern_ = ambNextOn(s.ambient.patternMask, pattern_);
  gfx->fillScreen(C_BLACK);
  rng_ = (uint32_t)millis() | 1u;
  gen_ = 0;
  phase_ = 0;

  if (pattern_ == PAT_LIFE) {
    // A third density gives long-lived soups; much more and it burns out fast,
    // much less and it dies before anything interesting forms.
    for (int y = 0; y < AMB_CELLS; y++)
      for (int x = 0; x < AMB_CELLS; x++) {
        cells_[y][x] = (xr(&rng_) % 100) < 33 ? 1 : 0;
        next_[y][x] = 0;
      }
    for (int y = 0; y < AMB_CELLS; y++)
      for (int x = 0; x < AMB_CELLS; x++)
        if (cells_[y][x])
          gfx->fillRect(x * AMB_CELL_PX, y * AMB_CELL_PX,
                        AMB_CELL_PX, AMB_CELL_PX, kLifeAge[0]);
  } else if (pattern_ == PAT_STARS) {
    for (int i = 0; i < AMB_STARS; i++) {
      stars_[i].x = (int16_t)(xr(&rng_) % 2000) - 1000;
      stars_[i].y = (int16_t)(xr(&rng_) % 2000) - 1000;
      stars_[i].z = (uint8_t)(1 + xr(&rng_) % 255);
      stars_[i].drawn = false;
    }
  } else if (pattern_ == PAT_RAIN) {
    for (int i = 0; i < AMB_COLS; i++) {
      rainY_[i] = -(int16_t)(xr(&rng_) % TFT_HEIGHT);
      rainSpd_[i] = (uint8_t)(2 + xr(&rng_) % 5);
    }
  } else if (pattern_ == PAT_SPARKS) {
    for (int i = 0; i < AMB_SPARKS; i++) {
      sparks_[i].life = 0;
      sparks_[i].px = sparks_[i].py = -1;   // nothing on screen to erase yet
    }
    sparkTimer_ = 0;
  }
  patternSince_ = millis();
}

// Conway, wrapped at the edges so gliders come back round rather than falling
// off. Only cells that actually change are repainted — on a 3600-cell board a
// settled pattern costs a handful of rectangles a frame instead of 3600.
void AmbientMode::stepLife() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  int changes = 0;
  for (int y = 0; y < AMB_CELLS; y++) {
    const int yUp = (y + AMB_CELLS - 1) % AMB_CELLS, yDn = (y + 1) % AMB_CELLS;
    for (int x = 0; x < AMB_CELLS; x++) {
      const int xL = (x + AMB_CELLS - 1) % AMB_CELLS, xR = (x + 1) % AMB_CELLS;
      const int n = (cells_[yUp][xL] ? 1 : 0) + (cells_[yUp][x] ? 1 : 0) +
                    (cells_[yUp][xR] ? 1 : 0) + (cells_[y][xL] ? 1 : 0) +
                    (cells_[y][xR] ? 1 : 0) + (cells_[yDn][xL] ? 1 : 0) +
                    (cells_[yDn][x] ? 1 : 0) + (cells_[yDn][xR] ? 1 : 0);
      const uint8_t cur = cells_[y][x];
      uint8_t nxt;
      if (cur) nxt = (n == 2 || n == 3) ? (cur < 4 ? cur + 1 : 4) : 0;
      else     nxt = (n == 3) ? 1 : 0;
      next_[y][x] = nxt;

      const bool wasOn = cur != 0, isOn = nxt != 0;
      const uint16_t wasC = wasOn ? kLifeAge[(cur - 1) & 3] : C_BLACK;
      const uint16_t isC  = isOn ? kLifeAge[(nxt - 1) & 3] : C_BLACK;
      if (wasC != isC) {
        gfx->fillRect(x * AMB_CELL_PX, y * AMB_CELL_PX,
                      AMB_CELL_PX, AMB_CELL_PX, isC);
        changes++;
      }
    }
  }
  memcpy(cells_, next_, sizeof(cells_));

  // A board that has stopped moving is not ambient, it is a still image. Reseed
  // rather than leave it, and cap the run so long oscillators still turn over.
  if (++gen_ > 900 || changes < 6) needFull_ = true;   // reseed the same pattern
}

// Classic sum-of-sines field, computed at half resolution and pushed a row at a
// time. The doubling happens while filling the line buffer, so the arithmetic
// is quartered without costing the panel any extra pixels.
void AmbientMode::stepPlasma() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  static uint16_t row[TFT_WIDTH];

  const float t = phase_ * 0.06f;
  for (int y = 0; y < TFT_HEIGHT; y += 2) {
    const float fy = y * 0.5f;
    for (int x = 0; x < TFT_WIDTH; x += 2) {
      const float fx = x * 0.5f;
      const float v = sinf(fx * 0.09f + t) +
                      sinf(fy * 0.11f - t * 0.7f) +
                      sinf((fx + fy) * 0.07f + t * 0.4f);
      // v is in [-3,3]; fold it onto a hue ramp without a full HSV conversion.
      const int k = (int)((v + 3.0f) * 42.0f) & 0xFF;
      const uint8_t r = (uint8_t)(128 + 127 * sinf(k * 0.0245f));
      const uint8_t g = (uint8_t)(128 + 127 * sinf(k * 0.0245f + 2.09f));
      const uint8_t b = (uint8_t)(128 + 127 * sinf(k * 0.0245f + 4.19f));
      const uint16_t c = ((uint16_t)(r & 0xF8) << 8) |
                         ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
      row[x] = c;
      if (x + 1 < TFT_WIDTH) row[x + 1] = c;
    }
    gfx->draw16bitRGBBitmap(0, y, row, TFT_WIDTH, 1);
    if (y + 1 < TFT_HEIGHT) gfx->draw16bitRGBBitmap(0, y + 1, row, TFT_WIDTH, 1);
  }
  phase_++;
}

// Stars flying at the viewer. Each one erases its own previous pixel, so the
// frame costs a hundred pixels rather than a full screen.
void AmbientMode::stepStars() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const int cx = TFT_WIDTH / 2, cy = TFT_HEIGHT / 2;

  for (int i = 0; i < AMB_STARS; i++) {
    Star& st = stars_[i];
    if (st.drawn) {
      const int sz = st.z < 80 ? 2 : 1;
      gfx->fillRect(st.px, st.py, sz, sz, C_BLACK);
      st.drawn = false;
    }
    if (st.z <= 3) {
      st.x = (int16_t)(xr(&rng_) % 2000) - 1000;
      st.y = (int16_t)(xr(&rng_) % 2000) - 1000;
      st.z = 255;
    } else {
      st.z -= 3;
    }
    const int px = cx + (st.x * 90) / st.z;
    const int py = cy + (st.y * 90) / st.z;
    if (px < 0 || px >= TFT_WIDTH - 1 || py < 0 || py >= TFT_HEIGHT - 1) {
      st.z = 3;                       // off the edge: respawn next frame
      continue;
    }
    // Nearer stars are bigger and brighter, which is the whole depth cue.
    const uint8_t lum = (uint8_t)(255 - st.z);
    const uint16_t c = ((uint16_t)(lum & 0xF8) << 8) |
                       ((uint16_t)(lum & 0xFC) << 3) | (lum >> 3);
    const int sz = st.z < 80 ? 2 : 1;
    gfx->fillRect(px, py, sz, sz, c);
    st.px = (uint8_t)px;
    st.py = (uint8_t)py;
    st.drawn = true;
  }
}

// Falling glyph columns. Each column only paints its new head and blanks the
// cell one tail-length behind it, so a frame is a few dozen small fills rather
// than a screen.
void AmbientMode::stepRain() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  const int tail = 14;
  gfx->setTextSize(1);
  for (int c = 0; c < AMB_COLS; c++) {
    const int x = c * 6;
    const int y = rainY_[c];
    if (y >= 0 && y < TFT_HEIGHT) {
      // The head is bright, the character one step back cools to green: two
      // draws gives the whole trail its gradient for free.
      gfx->setTextColor(0xFFFF);
      gfx->setCursor(x, y);
      gfx->write((char)(33 + xr(&rng_) % 90));
      if (y >= 8) {
        gfx->setTextColor(0x07E0);
        gfx->setCursor(x, y - 8);
        gfx->write((char)(33 + xr(&rng_) % 90));
      }
    }
    const int erase = y - tail * 8;
    if (erase >= 0 && erase < TFT_HEIGHT) gfx->fillRect(x, erase, 6, 8, C_BLACK);

    rainY_[c] += rainSpd_[c] + 4;
    if (rainY_[c] - tail * 8 >= TFT_HEIGHT) {
      rainY_[c] = -(int16_t)(xr(&rng_) % 60);
      rainSpd_[c] = (uint8_t)(2 + xr(&rng_) % 5);
    }
  }
}

// Dim an RGB565 colour toward black by shifting each channel down. Used for the
// tail of a particle's life, so a shell settles instead of switching off.
static inline uint16_t dim565(uint16_t c, uint8_t shift) {
  const uint16_t r = ((c >> 11) & 0x1F) >> shift;
  const uint16_t g = ((c >> 5) & 0x3F) >> shift;
  const uint16_t b = (c & 0x1F) >> shift;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Fireworks. Each particle erases exactly the block it left, so a burst costs a
// few hundred pixels a frame rather than a repaint.
//
// The shape is the whole point and the old version got it wrong: velocities
// came from two independent `rand() % 15` draws, which fills a *square*, not a
// ring — the corners are 1.4x faster than the axes and the interior is full of
// near-stationary particles. A real shell is a spherical burst seen flat, so
// emit on a circle: even angular spread, one speed for the shell with a little
// jitter, and gravity to pull the ring into a droop.
void AmbientMode::stepSparks() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;

  if (sparkTimer_) sparkTimer_--;
  if (!sparkTimer_) {
    sparkTimer_ = (uint16_t)(30 + xr(&rng_) % 45);
    const int bx = 50 + (int)(xr(&rng_) % (TFT_WIDTH - 100));
    const int by = 45 + (int)(xr(&rng_) % (TFT_HEIGHT - 130));
    // One hue per shell reads as a firework; mixed colours read as confetti.
    static const uint16_t kHue[6] = {0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x05FF, 0xF81F};
    const uint16_t col = kHue[xr(&rng_) % 6];
    // 2.2..3.6 px per frame, in 8.8. Big enough to throw a visible ring in half
    // a second, small enough that the ring stays a ring while it expands.
    const int32_t speed = 560 + (int32_t)(xr(&rng_) % 360);
    const float   spin  = (float)(xr(&rng_) % 628) * 0.01f;   // random ring phase
    const uint8_t life  = (uint8_t)(30 + xr(&rng_) % 16);
    int made = 0;
    for (int i = 0; i < AMB_SPARKS && made < AMB_BURST; i++) {
      if (sparks_[i].life) continue;
      Spark& p = sparks_[i];
      const float a = spin + (float)made * (6.2832f / AMB_BURST);
      // +/- 12% on the radius keeps the ring from looking like a stencil.
      const int32_t sp = speed * (int32_t)(88 + xr(&rng_) % 25) / 100;
      p.x = (int32_t)bx << 8;
      p.y = (int32_t)by << 8;
      p.vx = (int16_t)(cosf(a) * sp);
      p.vy = (int16_t)(sinf(a) * sp);
      p.px = p.py = -1;
      p.life = life;
      p.age = 0;
      p.col = col;
      made++;
    }
  }

  for (int i = 0; i < AMB_SPARKS; i++) {
    Spark& p = sparks_[i];
    if (!p.life) continue;

    if (p.px >= 0)
      gfx->fillRect(p.px, p.py, AMB_SPARK_PX, AMB_SPARK_PX, C_BLACK);

    p.x += p.vx;
    p.y += p.vy;
    p.vy = (int16_t)(p.vy + 14);                    // gravity, 8.8
    p.vx = (int16_t)(p.vx - (p.vx >> 5));           // a little air drag
    p.vy = (int16_t)(p.vy - (p.vy >> 5));
    p.age++;

    const int ix = p.x >> 8, iy = p.y >> 8;
    if (--p.life == 0 || ix < 0 || ix > TFT_WIDTH - AMB_SPARK_PX ||
        iy < 0 || iy > TFT_HEIGHT - AMB_SPARK_PX) {
      p.life = 0;
      p.px = p.py = -1;
      continue;
    }

    // Bright while it climbs, cooling over the last third: a shell that fades
    // reads as burning out, one that vanishes reads as a dropped frame.
    const uint16_t c = p.life > 12 ? p.col
                     : p.life > 6  ? dim565(p.col, 1)
                                   : dim565(p.col, 2);
    gfx->fillRect(ix, iy, AMB_SPARK_PX, AMB_SPARK_PX, c);
    p.px = (int16_t)ix;
    p.py = (int16_t)iy;
  }
}

void AmbientMode::service(const Settings& s) {
  if (!gfxDev()) return;

  if (needFull_) {
    needFull_ = false;
    startPattern(s);
    lastMs_ = millis();
    return;
  }

  // Each pattern has its own natural pace. Life wants to be readable, the
  // starfield wants to be smooth, and the plasma runs as fast as the bus allows.
  const uint32_t period = (pattern_ == PAT_LIFE)   ? 110
                        : (pattern_ == PAT_STARS)  ? 33
                        : (pattern_ == PAT_RAIN)   ? 60
                        : (pattern_ == PAT_SPARKS) ? 33 : 0;
  const uint32_t now = millis();
  if (period && now - lastMs_ < period) return;
  lastMs_ = now;

  switch (pattern_) {
    case PAT_LIFE:   stepLife();   break;
    case PAT_PLASMA: stepPlasma(); break;
    case PAT_RAIN:   stepRain();   break;
    case PAT_SPARKS: stepSparks(); break;
    default:         stepStars();  break;
  }

  // Three minutes of one pattern is a screensaver that has stopped being
  // interesting; rotate inside the block.
  if (s.ambient.patternSec &&
      now - patternSince_ >= (uint32_t)s.ambient.patternSec * 1000UL) {
    nextPattern(s);
  }
}

#endif  // WITH_AMBIENT
