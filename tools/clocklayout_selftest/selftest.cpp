// Host-side checks for src/features/clock/ClockLayout.h — where the big digits
// and the AM/PM sit.
//
// This exists because the bug it guards against was invisible: the digits were
// sized to fill the panel and the suffix was then drawn on top at a fixed right
// margin, so they overlapped — but only in 12h mode, and only at times whose
// digits happened to be wide enough. "11:36" ran into its own "PM" while
// "1:36" looked perfectly fine. A static_assert cannot catch that; the overlap
// depends on the string.
#include <cstdio>
#include <cstring>

#define TFT_WIDTH 240
#include "../../src/features/clock/ClockLayout.h"

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}

// Every HH:MM a clock can show, in both modes.
struct Case { const char* s; int suffix; };

static bool overlaps(const ClockLayout& L, int digitsLen, int suffixLen) {
  if (!suffixLen) return false;
  const int digitsEnd = L.digitsX + clockTextW(digitsLen, L.size);
  return L.suffixX < digitsEnd;
}

int main() {
  printf("--- the reported failure ------------------------------------\n");
  {
    // 11:36 PM, the exact case that was reported off the device.
    const ClockLayout L = clockLayout(5, 2, TFT_WIDTH, 78);
    const int digitsEnd = L.digitsX + clockTextW(5, L.size);
    printf("        size %u, digits x=%d..%d, PM at x=%d\n",
           L.size, L.digitsX, digitsEnd, L.suffixX);
    ck(!overlaps(L, 5, 2), "\"11:36\" does not run into its own PM");
    ck(L.suffixX >= digitsEnd + CLOCK_SUFFIX_GAP, "and keeps the gap");
  }

  printf("\n--- every time of day ---------------------------------------\n");
  {
    // 24h mode is always "HH:MM"; 12h is "H:MM" or "HH:MM".
    bool anyOverlap = false, offPanel = false, tooSmall = false;
    int minSize = 99;
    for (int h = 0; h < 24; h++) {
      for (int m = 0; m < 60; m++) {
        char buf[8];
        // 24h
        snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
        ClockLayout L = clockLayout((int)strlen(buf), 0, TFT_WIDTH, 78);
        if (L.digitsX < 0 || L.digitsX + clockTextW((int)strlen(buf), L.size) > TFT_WIDTH)
          offPanel = true;
        if (L.size < minSize) minSize = L.size;

        // 12h
        int hh = h % 12; if (!hh) hh = 12;
        snprintf(buf, sizeof(buf), "%d:%02d", hh, m);
        const int len = (int)strlen(buf);
        L = clockLayout(len, 2, TFT_WIDTH, 78);
        if (overlaps(L, len, 2)) anyOverlap = true;
        const int blockEnd = L.suffixX + clockTextW(2, CLOCK_SUFFIX_SIZE);
        if (L.digitsX < 0 || blockEnd > TFT_WIDTH) offPanel = true;
        if (L.size < minSize) minSize = L.size;
        if (L.size < 4) tooSmall = true;
      }
    }
    ck(!anyOverlap, "no time of day overlaps its AM/PM");
    ck(!offPanel, "nothing is drawn off either edge of the panel");
    ck(!tooSmall, "the digits never shrink below size 4");
    printf("        smallest size used across all 2880 times: %d\n", minSize);
  }

  printf("\n--- centring -------------------------------------------------\n");
  {
    // 24h has no suffix, so the digits themselves should be centred.
    const ClockLayout L = clockLayout(5, 0, TFT_WIDTH, 78);
    const int left = L.digitsX;
    const int right = TFT_WIDTH - (L.digitsX + clockTextW(5, L.size));
    ck(left - right <= 1 && right - left <= 1, "24h digits are centred");
  }
  {
    // 12h centres digits+suffix as a block, so the whole thing is centred even
    // though the digits alone sit left of centre.
    const int len = 5;
    const ClockLayout L = clockLayout(len, 2, TFT_WIDTH, 78);
    const int blockEnd = L.suffixX + clockTextW(2, CLOCK_SUFFIX_SIZE);
    const int left = L.digitsX, right = TFT_WIDTH - blockEnd;
    ck(left - right <= 1 && right - left <= 1, "12h centres digits and suffix together");
    ck(L.digitsX > 0, "and still leaves a left margin");
  }

  printf("\n--- vertical -------------------------------------------------\n");
  {
    const ClockLayout L = clockLayout(5, 2, TFT_WIDTH, 78);
    ck(L.digitsY + L.digitsH / 2 == 78 || L.digitsY + L.digitsH / 2 == 77,
       "digits are centred on the requested line");
    ck(L.suffixY >= L.digitsY, "the suffix sits inside the digit band");
    ck(L.suffixY + 8 * CLOCK_SUFFIX_SIZE <= L.digitsY + L.digitsH,
       "and does not hang below it");
    // The clear band is fillRect(0, digitsY-4, w, digitsH+8) in ClockMode; the
    // seconds bar starts at y=128, so the two must not collide.
    ck(L.digitsY + L.digitsH + 4 <= 128, "the cleared band does not reach the seconds bar");
  }

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
