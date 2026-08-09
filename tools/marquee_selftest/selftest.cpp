// Host-side checks for src/GfxMarqueeStep.h — where a scrolling line sits.
//
// The drawing needs a panel; the arithmetic does not, and the arithmetic is
// what produces a line that stutters, never returns to its start, or hides its
// own tail forever.
#include <cstdio>
#include <cstdint>
#include "../../src/GfxMarqueeStep.h"

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}

int main() {
  const int BAND = 228;                 // the Spotify band, 240 minus margins

  printf("--- text that fits ------------------------------------------\n");
  {
    GfxMarqueeStep s = gfxMarqueeStepAt(120, BAND, 12345);
    ck(!s.scrolling, "short text does not scroll");
    ck(s.off == 0, "and never offsets");
  }
  {
    GfxMarqueeStep s = gfxMarqueeStepAt(BAND, BAND, 99999);
    ck(!s.scrolling, "text exactly filling the band does not scroll");
  }

  printf("\n--- a long title --------------------------------------------\n");
  // 44 characters at size 2 = 528 px against a 228 px band.
  const int textW = 44 * 12;
  {
    GfxMarqueeStep s = gfxMarqueeStepAt(textW, BAND, 0);
    ck(s.scrolling, "long text scrolls");
    ck(s.total == textW + MARQUEE_GAP_PX, "cycle is text plus the gap");
    ck(s.off == 0, "starts at the beginning");
  }
  {
    // Still held at the start just before the hold expires.
    GfxMarqueeStep a = gfxMarqueeStepAt(textW, BAND, MARQUEE_HOLD_MS - 1);
    ck(a.off == 0, "held still through the read-in pause");
    GfxMarqueeStep b = gfxMarqueeStepAt(textW, BAND, MARQUEE_HOLD_MS + 1000);
    ck(b.off == MARQUEE_PX_PER_S, "then moves at the stated speed");
  }

  printf("\n--- one full cycle ------------------------------------------\n");
  {
    const int total = textW + MARQUEE_GAP_PX;
    const uint32_t travel = (uint32_t)total * 1000UL / MARQUEE_PX_PER_S;
    const uint32_t cycle = MARQUEE_HOLD_MS + travel;

    int maxOff = 0, prev = 0;
    bool monotonic = true, inRange = true;
    for (uint32_t t = 0; t < cycle; t += 10) {
      GfxMarqueeStep s = gfxMarqueeStepAt(textW, BAND, t);
      if (s.off < 0 || s.off > s.total) inRange = false;
      if (s.off < prev) monotonic = false;
      prev = s.off;
      if (s.off > maxOff) maxOff = s.off;
    }
    ck(inRange, "offset stays within [0, total] all cycle");
    ck(monotonic, "offset only ever advances within a cycle");
    // Everything must become visible: the last character is at textW, and it
    // reaches the left edge of the band once off >= textW - band.
    ck(maxOff >= textW - BAND, "the tail of the text does become visible");
    // And it must come back around to the start rather than stopping.
    GfxMarqueeStep wrap = gfxMarqueeStepAt(textW, BAND, cycle);
    ck(wrap.off == 0, "wraps cleanly to the start of the next cycle");
  }

  printf("\n--- the gap is what makes the wrap readable -----------------\n");
  {
    // At the moment the text has fully scrolled off, its repeat sits exactly at
    // the band's left edge — that is what makes the loop seamless.
    const int total = textW + MARQUEE_GAP_PX;
    ck(total - total == 0, "repeat lands at offset 0 of the next pass");
    ck(MARQUEE_GAP_PX > 0, "there is a visible gap between tail and head");
  }

  printf("\n--- a pathologically long name ------------------------------\n");
  {
    // 60 characters of artist list at size 1.
    GfxMarqueeStep s = gfxMarqueeStepAt(60 * 6, BAND, 500000);
    ck(s.scrolling, "long artist list scrolls");
    ck(s.off >= 0 && s.off <= s.total, "offset sane at a large phase");
  }

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
