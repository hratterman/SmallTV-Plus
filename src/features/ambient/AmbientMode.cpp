#include "config.h"
#if WITH_AMBIENT

#include "AmbientMode.h"
#include <Arduino_GFX_Library.h>
#include "Gfx.h"
#include "Settings.h"
#include <math.h>

AmbientMode g_ambientMode;

#define AMB_PATTERNS 3
#define PAT_LIFE   0
#define PAT_PLASMA 1
#define PAT_STARS  2

// A cell's colour by how long it has been alive: new cells are bright, settled
// ones cool off. Turns a binary automaton into something with depth.
static const uint16_t kLifeAge[4] = {0x07FF, 0x05BF, 0x033F, 0x01DF};

static inline uint32_t xr(uint32_t* s) {
  uint32_t v = *s;
  v ^= v << 13; v ^= v >> 17; v ^= v << 5;
  return *s = v;
}

void AmbientMode::begin(const Settings& s) { invalidate(s); }

void AmbientMode::invalidate(const Settings& s) {
  needFull_ = true;
  pattern_ = 0;
}

void AmbientMode::onContextAction(Settings& s) {
  pattern_ = (uint8_t)((pattern_ + 1) % AMB_PATTERNS);
  needFull_ = true;
}

void AmbientMode::startPattern(const Settings& s) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
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
  }
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
  if (++gen_ > 900 || changes < 6) {
    pattern_ = PAT_LIFE;
    needFull_ = true;
  }
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
  const uint32_t period = (pattern_ == PAT_LIFE) ? 110
                        : (pattern_ == PAT_STARS) ? 33 : 0;
  const uint32_t now = millis();
  if (period && now - lastMs_ < period) return;
  lastMs_ = now;

  switch (pattern_) {
    case PAT_LIFE:   stepLife();   break;
    case PAT_PLASMA: stepPlasma(); break;
    default:         stepStars();  break;
  }
}

#endif  // WITH_AMBIENT
