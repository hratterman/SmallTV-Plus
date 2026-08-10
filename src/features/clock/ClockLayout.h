// ClockLayout.h — where the big digits and the AM/PM sit.
//
// Split out of ClockMode.cpp because it is pure arithmetic over string lengths,
// and because getting it wrong is invisible until a particular time of day.
// The original sized the digits to fill the whole panel and then drew the
// suffix on top at a fixed right margin, so in 12h mode they overlapped: at
// 11:36 the centred digits reached x=225 and "PM" started at x=206.
//
// The rule now: the suffix's width comes out of the budget before the digits
// are sized, and the two are centred together as one block so the clock does
// not sit visibly off-centre in 12h mode.
#pragma once
#include <stdint.h>

#define CLOCK_SUFFIX_SIZE 2     // text size for AM/PM
#define CLOCK_SUFFIX_GAP  6     // px between the digits and the suffix
#define CLOCK_SIDE_MARGIN 24    // px kept clear at both edges
#define CLOCK_MAX_SIZE    8     // largest text size the digits may use

// The built-in 5x7 font with its 1 px spacing: every glyph is 6*size wide.
static inline int clockTextW(int len, int size) { return len * 6 * size; }

struct ClockLayout {
  uint8_t size;             // text size for the digits
  int digitsX, digitsY;     // top-left of the digits
  int digitsH;              // digit height, for the band that gets cleared
  int suffixX, suffixY;     // top-left of AM/PM (undefined when suffixLen == 0)
};

// digitsLen: characters in "HH:MM". suffixLen: 0 for 24h, else 2 for AM/PM.
// centerY: the vertical middle the digits are centred on.
static inline ClockLayout clockLayout(int digitsLen, int suffixLen,
                                      int panelW, int centerY) {
  ClockLayout L;
  const int suffixW =
      suffixLen ? clockTextW(suffixLen, CLOCK_SUFFIX_SIZE) + CLOCK_SUFFIX_GAP : 0;

  // Largest size whose digits still fit beside the suffix.
  const int budget = panelW - CLOCK_SIDE_MARGIN - suffixW;
  L.size = 1;
  for (int sz = CLOCK_MAX_SIZE; sz > 1; sz--) {
    if (clockTextW(digitsLen, sz) <= budget) { L.size = (uint8_t)sz; break; }
  }

  const int digitsW = clockTextW(digitsLen, L.size);
  L.digitsH = 8 * L.size;
  L.digitsY = centerY - L.digitsH / 2;

  L.digitsX = (panelW - (digitsW + suffixW)) / 2;
  if (L.digitsX < 0) L.digitsX = 0;

  L.suffixX = L.digitsX + digitsW + CLOCK_SUFFIX_GAP;
  // Sit the suffix on the bottom of the digits rather than the top.
  L.suffixY = L.digitsY + L.digitsH - 8 * CLOCK_SUFFIX_SIZE;
  return L;
}
