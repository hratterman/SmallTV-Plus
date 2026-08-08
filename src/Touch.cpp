#include "Touch.h"
#if HAS_TOUCH

// The ESP32's touch channels are fixed to particular pins. Four of the ten are
// already spoken for by the display (GPIO 2/13/14/15 are DC/MOSI/SCLK/CS), so
// only these six can carry the pad. Reading a display pin as a touch channel
// would fight the panel, so they are never probed.
static const uint8_t kCandidates[TOUCH_MAX_CHANNELS] = {4, 0, 12, 27, 33, 32};

// Untouched readings drift with temperature and humidity, so the baseline is a
// slow moving average taken only while the pad is released. A touch pulls the
// reading down sharply, far faster than the baseline can follow.
static uint16_t s_baseline[TOUCH_MAX_CHANNELS];
static bool     s_primed = false;
static int8_t   s_activeIdx = -1;      // index into kCandidates, -1 = unconfigured

// Gesture state
enum : uint8_t { ST_IDLE, ST_PRESSED, ST_MAYBE_TAP, ST_WAIT_RELEASE };
static uint8_t  s_state = ST_IDLE;
static uint32_t s_stateAt = 0;
static uint32_t s_lastSample = 0;
static uint8_t  s_debounce = 0;

#define TOUCH_SAMPLE_MS   25
#define TOUCH_LONG_MS    700
#define TOUCH_DOUBLE_MS  350
#define TOUCH_DEBOUNCE     2      // consecutive samples before a state change

static int candidateIndex(int gpio) {
  for (uint8_t i = 0; i < TOUCH_MAX_CHANNELS; i++)
    if (kCandidates[i] == gpio) return i;
  return -1;
}

static uint16_t readChannel(uint8_t gpio) {
  uint32_t v = touchRead(gpio);
  if (v > 0xFFFF) v = 0xFFFF;
  return (uint16_t)v;
}

static void primeBaselines() {
  for (uint8_t i = 0; i < TOUCH_MAX_CHANNELS; i++) s_baseline[i] = readChannel(kCandidates[i]);
  s_primed = true;
}

void touchBegin(const Settings& s) {
  primeBaselines();
  touchInvalidate(s);
}

void touchInvalidate(const Settings& s) {
  s_activeIdx = (s.touch.gpio >= 0) ? candidateIndex(s.touch.gpio) : -1;
  s_state = ST_IDLE;
  s_debounce = 0;
}

// Single-channel read + baseline maintenance for the configured pad.
static bool sampleActive(const Settings& s) {
  const uint8_t idx = (uint8_t)s_activeIdx;
  const uint16_t v = readChannel(kCandidates[idx]);
  const int16_t delta = (int16_t)s_baseline[idx] - (int16_t)v;
  const bool pressed = delta >= (int16_t)s.touch.threshold;

  // Track the baseline only while released, and only slowly, so a finger
  // resting on the pad can never train the baseline onto itself.
  if (!pressed) s_baseline[idx] = (uint16_t)((s_baseline[idx] * 15 + v) / 16);
  return pressed;
}

TouchEvent touchService(const Settings& s) {
  if (!s.touch.enabled || s_activeIdx < 0) return TOUCH_NONE;

  const uint32_t now = millis();

  // A pending single tap becomes real once the double-tap window closes; this
  // has to be checked between samples, not only on one.
  if (s_state == ST_MAYBE_TAP && (now - s_stateAt) >= TOUCH_DOUBLE_MS) {
    s_state = ST_IDLE;
    return TOUCH_TAP;
  }

  if (now - s_lastSample < TOUCH_SAMPLE_MS) return TOUCH_NONE;
  s_lastSample = now;
  if (!s_primed) { primeBaselines(); return TOUCH_NONE; }

  const bool raw = sampleActive(s);

  // Debounce: require a few consecutive agreeing samples before believing an edge.
  static bool s_stable = false;
  if (raw != s_stable) {
    if (++s_debounce >= TOUCH_DEBOUNCE) { s_stable = raw; s_debounce = 0; }
  } else {
    s_debounce = 0;
  }
  const bool pressed = s_stable;

  switch (s_state) {
    case ST_IDLE:
      if (pressed) { s_state = ST_PRESSED; s_stateAt = now; }
      break;

    case ST_PRESSED:
      if (!pressed) {
        // Short press: hold it back in case a second one follows.
        s_state = ST_MAYBE_TAP;
        s_stateAt = now;
      } else if (now - s_stateAt >= TOUCH_LONG_MS) {
        s_state = ST_WAIT_RELEASE;
        return TOUCH_LONG;
      }
      break;

    case ST_MAYBE_TAP:
      if (pressed) {
        s_state = ST_WAIT_RELEASE;
        return TOUCH_DOUBLE;
      }
      break;

    case ST_WAIT_RELEASE:
      if (!pressed) s_state = ST_IDLE;
      break;
  }
  return TOUCH_NONE;
}

void touchDiag(TouchDiag& out) {
  if (!s_primed) primeBaselines();
  out.count = TOUCH_MAX_CHANNELS;
  for (uint8_t i = 0; i < TOUCH_MAX_CHANNELS; i++) {
    const uint16_t v = readChannel(kCandidates[i]);
    out.gpio[i]     = kCandidates[i];
    out.raw[i]      = v;
    out.baseline[i] = s_baseline[i];
    out.delta[i]    = (int16_t)s_baseline[i] - (int16_t)v;
    out.pressed[i]  = out.delta[i] > 12;
    // Same rule as the live path: only learn while apparently untouched.
    if (!out.pressed[i]) s_baseline[i] = (uint16_t)((s_baseline[i] * 15 + v) / 16);
  }
}

int touchDetect(uint16_t ms) {
  if (!s_primed) primeBaselines();

  // Settle first: whatever the baselines were, re-establish them now, since the
  // user may have been resting a hand nearby.
  for (uint8_t i = 0; i < TOUCH_MAX_CHANNELS; i++) s_baseline[i] = readChannel(kCandidates[i]);

  int16_t peak[TOUCH_MAX_CHANNELS] = {0};
  const uint32_t until = millis() + ms;
  while ((int32_t)(millis() - until) < 0) {
    for (uint8_t i = 0; i < TOUCH_MAX_CHANNELS; i++) {
      const int16_t d = (int16_t)s_baseline[i] - (int16_t)readChannel(kCandidates[i]);
      if (d > peak[i]) peak[i] = d;
    }
    delay(10);
  }

  // Pick the largest excursion, but only if it clearly beats both the noise
  // floor and the runner-up — a pad tap should be unambiguous, and neighbouring
  // channels pick up a little of it.
  int best = -1, second = -1;
  for (uint8_t i = 0; i < TOUCH_MAX_CHANNELS; i++) {
    if (best < 0 || peak[i] > peak[best]) { second = best; best = i; }
    else if (second < 0 || peak[i] > peak[second]) { second = i; }
  }
  if (best < 0 || peak[best] < 15) return -1;
  if (second >= 0 && peak[best] < peak[second] * 2) return -1;
  return kCandidates[best];
}

#endif  // HAS_TOUCH
