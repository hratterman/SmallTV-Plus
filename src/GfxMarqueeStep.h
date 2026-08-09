// GfxMarqueeStep.h — where a scrolling line sits at a given moment.
//
// Split out from the drawing so it can be checked on a host
// (tools/marquee_selftest). The drawing is hard to get wrong in a way that
// matters; the phase arithmetic is easy to get wrong in a way that shows up as
// a line that stutters, never returns to the start, or hides its own tail.
#pragma once
#include <stdint.h>

// Blank run between the end of the text and the start of its repeat, so a
// wrapped line reads as "…end   start…" instead of its tail touching its head.
#define MARQUEE_GAP_PX   28
// Held still at the start of each pass: a line that moves the instant it
// appears is hard to start reading.
#define MARQUEE_HOLD_MS  1600
#define MARQUEE_PX_PER_S 34

struct GfxMarqueeStep {
  bool scrolling;   // false when the text fits and should just be centred
  int  off;         // pixels the text has advanced left, 0..total
  int  total;       // one full cycle: text width plus the gap
};

// `phaseMs` is a free-running clock; pass millis().
static inline GfxMarqueeStep gfxMarqueeStepAt(int textW, int bandW, uint32_t phaseMs) {
  GfxMarqueeStep r;
  r.scrolling = textW > bandW;
  r.total = textW + MARQUEE_GAP_PX;
  r.off = 0;
  if (!r.scrolling) return r;

  const uint32_t travelMs = (uint32_t)r.total * 1000UL / MARQUEE_PX_PER_S;
  const uint32_t cycle = MARQUEE_HOLD_MS + travelMs;
  const uint32_t t = phaseMs % cycle;
  if (t > MARQUEE_HOLD_MS)
    r.off = (int)((uint32_t)(t - MARQUEE_HOLD_MS) * MARQUEE_PX_PER_S / 1000UL);
  if (r.off > r.total) r.off = r.total;
  return r;
}
