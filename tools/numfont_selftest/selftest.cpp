// Host-side checks for the generated font headers: src/NumFonts.h (the
// device-wide numbers faces) and the clock's font_clock_sans.h.
//
// Generated font data has no way to complain — a bitmap offset past the end of
// the array reads garbage from whatever is next in flash, and a glyph taller
// than its face's ascent+descent quietly scribbles on the band above. Both are
// invisible until a particular digit is on a particular screen.
#include <cstdio>
#include <cstring>

#include "../../src/NumFonts.h"
#include "../../src/features/clock/font_clock_sans.h"

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}

static int textW(const GFXfont* f, const char* s) {
  int w = 0;
  for (const char* p = s; *p; p++) {
    if ((uint8_t)*p < f->first || (uint8_t)*p > f->last) return -1;
    w += f->glyph[(uint8_t)*p - f->first].xAdvance;
  }
  return w;
}

static void checkFont(const char* name, const GFXfont* f, int bitmapLen,
                      int ascent, int descent) {
  const int n = f->last - f->first + 1;
  bool inBounds = true, heights = true, advances = true;
  for (int i = 0; i < n; i++) {
    const GFXglyph& g = f->glyph[i];
    const int bytes = (g.width * g.height + 7) / 8;
    if (g.width && g.bitmapOffset + bytes > bitmapLen) inBounds = false;
    if (-g.yOffset > ascent || g.yOffset + g.height > descent) heights = false;
    if (g.xAdvance == 0 && (f->first + i) != ' ') advances = false;
  }
  char what[96];
  snprintf(what, sizeof(what), "%s: every bitmap stays inside its array", name);
  ck(inBounds, what);
  snprintf(what, sizeof(what), "%s: every glyph fits ascent %d + descent %d",
           name, ascent, descent);
  ck(heights, what);
  snprintf(what, sizeof(what), "%s: no zero-width advances (except space)", name);
  ck(advances, what);

  // Monospaced digits: the whole point of the generator's post-pass.
  int dadv = f->glyph['0' - f->first].xAdvance;
  bool mono = true;
  for (char c = '0'; c <= '9'; c++)
    if (f->glyph[c - f->first].xAdvance != dadv) mono = false;
  snprintf(what, sizeof(what), "%s: digits are monospaced at %d px", name, dadv);
  ck(mono, what);
}

int main() {
  printf("--- structure ------------------------------------------------\n");
  checkFont("NumSansBig", &NumSansBig, (int)sizeof(NumSansBigBitmaps),
            kNumFaces[0].ascent, kNumFaces[0].descent);
  checkFont("NumSansMid", &NumSansMid, (int)sizeof(NumSansMidBitmaps),
            kNumFaces[1].ascent, kNumFaces[1].descent);
  checkFont("NumSansSmall", &NumSansSmall, (int)sizeof(NumSansSmallBitmaps),
            kNumFaces[2].ascent, kNumFaces[2].descent);
  checkFont("ClockSans", &ClockSans, (int)sizeof(ClockSansBitmaps),
            CLOCK_SANS_ASCENT, CLOCK_SANS_DESCENT);

  printf("\n--- the faces get smaller, in order --------------------------\n");
  ck(kNumFaces[0].ascent > kNumFaces[1].ascent &&
         kNumFaces[1].ascent > kNumFaces[2].ascent,
     "big > mid > small, so the auto-fit walk makes sense");

  printf("\n--- every site's worst case fits some face -------------------\n");
  {
    // Ticker price band: 236 px. Longest realistic price with the $ prefix.
    const char* worst = "$99,999.99";
    bool fits = false;
    for (int i = 0; i < NUM_FONT_COUNT && !fits; i++) {
      const int w = textW(kNumFaces[i].font, worst);
      if (w > 0 && w <= 236) fits = true;
    }
    ck(fits, "'$99,999.99' fits the price band in some face");
    ck(textW(kNumFaces[0].font, "$313.33") <= 236,
       "a normal price gets the big face");
  }
  {
    // Change line: 200 px beside the arrow.
    const char* worst = "+999.99 (+99.99%)";
    bool fits = false;
    for (int i = 0; i < NUM_FONT_COUNT && !fits; i++)
      if (textW(kNumFaces[i].font, worst) <= 200 &&
          textW(kNumFaces[i].font, worst) > 0) fits = true;
    ck(fits, "a worst-case change line fits beside its arrow");
  }
  {
    // Usage: "100%" in 150 px, height inside the 42 px before the bar.
    const int w = textW(kNumFaces[0].font, "100%");
    ck(w > 0 && w <= 150, "'100%' fits the meter in the big face");
    ck(kNumFaces[0].ascent + 2 <= 43,
       "and the big face's ascent fits above the meter bar");
    // The site positions by ascent alone, which is only sound if the glyphs a
    // percentage can contain never dip below the baseline.
    bool noDescender = true;
    for (const char* p = "0123456789%"; *p; p++) {
      const GFXglyph& g = kNumFaces[0].font->glyph[(uint8_t)*p - kNumFaces[0].font->first];
      if (g.yOffset + g.height > 2) noDescender = false;
    }
    ck(noDescender, "digits and % never descend more than 2 px in the big face");
  }
  {
    // Miner: five digits and a dot in 150 px.
    const int w = textW(kNumFaces[0].font, "999.9");
    ck(w > 0 && w <= 150, "'999.9' hashrate fits in the big face");
  }
  {
    // The clock: "88:88" plus the AM/PM budget inside 240.
    const int w = 4 * CLOCK_SANS_DIGIT_ADV + CLOCK_SANS_COLON_ADV;
    ck(w + 6 + 24 <= 240, "'88:88' plus AM/PM fits the panel in ClockSans");
  }

  printf("\n--- eligibility matches the charset --------------------------\n");
  {
    bool ok = true;
    for (int c = NUM_FONT_FIRST; c <= NUM_FONT_LAST; c++) {
      char one[2] = {(char)c, 0};
      if (textW(kNumFaces[0].font, one) < 0) ok = false;
    }
    ck(ok, "every charset character resolves to a glyph");
    ck(textW(kNumFaces[0].font, "kH/s") < 0,
       "letters do not (they stay in the pixel font)");
  }

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
