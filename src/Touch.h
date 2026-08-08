// Touch.h — the capacitive pad on top of the cube.
//
// The stock NMMiner firmware used this pad to change pages, so it is wired and
// readable, but nothing documents which GPIO it lands on. The ESP32 has native
// touch sensing on ten specific pins; six of them are free on this board (the
// rest drive the display), so rather than ship a diagnostic build and wait a
// round trip, the firmware reads all six and the web UI lets you find the pad by
// tapping it — see touchDiag() and touchDetect().
//
// On top of that sits a small gesture layer with one grammar shared by every
// mode: tap advances the mode, double-tap blanks the screen, long-press is the
// active mode's own action.
#pragma once
#include <Arduino.h>
#include "config.h"
#include "Settings.h"

#if HAS_TOUCH

enum TouchEvent : uint8_t {
  TOUCH_NONE = 0,
  TOUCH_TAP,
  TOUCH_DOUBLE,
  TOUCH_LONG,
};

// Candidate channels, in the order the UI shows them.
#define TOUCH_MAX_CHANNELS 6
struct TouchDiag {
  uint8_t  count;
  uint8_t  gpio[TOUCH_MAX_CHANNELS];
  uint16_t raw[TOUCH_MAX_CHANNELS];
  uint16_t baseline[TOUCH_MAX_CHANNELS];
  int16_t  delta[TOUCH_MAX_CHANNELS];   // baseline - raw; a touch pushes it up
  bool     pressed[TOUCH_MAX_CHANNELS];
};

void       touchBegin(const Settings& s);
void       touchInvalidate(const Settings& s);   // config changed
TouchEvent touchService(const Settings& s);      // call every loop tick

// Live values for every candidate channel (used by the setup page).
void touchDiag(TouchDiag& out);

// Watch every candidate for `ms` and return the GPIO that deviated most, or -1
// if nothing moved convincingly. Blocking; the setup page calls it while the
// user taps the pad.
int touchDetect(uint16_t ms);

#endif  // HAS_TOUCH
