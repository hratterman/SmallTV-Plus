// Host-side checks for src/features/ambient/AmbientPick.h — which ambient
// pattern runs next.
//
// Only 32 possible masks, so nothing here samples: every case is checked
// against every starting pattern and every random draw. The two failures worth
// catching are a pattern the user unticked appearing anyway, and a selection
// loop that never terminates because it keeps looking for a different pattern
// when only one is ticked.
#include <cstdio>

#include "../../src/features/ambient/AmbientPick.h"

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}

static const char* NAMES[AMB_PATTERNS] = {"life", "plasma", "stars", "rain", "fireworks"};

int main() {
  const int MASKS = 1 << AMB_PATTERNS;

  printf("--- the mask itself ------------------------------------------\n");
  ck(AMB_PATTERN_ALL == 0x1F, "five patterns, all-on mask is 0x1f");
  ck(ambMaskOrAll(0) == AMB_PATTERN_ALL, "an empty mask is read as all of them");
  ck(ambMaskOrAll(0xFF) == AMB_PATTERN_ALL, "bits above the last pattern are ignored");
  ck(ambCountOn(0) == AMB_PATTERNS, "and an empty mask counts as all of them");
  ck(ambCountOn(1 << AMB_PAT_RAIN) == 1, "a single tick counts as one");

  printf("\n--- never shows an unticked pattern --------------------------\n");
  {
    bool nextOk = true, shufOk = true, firstOk = true;
    for (int mask = 1; mask < MASKS; mask++) {
      for (int cur = 0; cur < AMB_PATTERNS; cur++) {
        if (!ambPatternOn((uint8_t)mask, ambNextOn((uint8_t)mask, (uint8_t)cur)))
          nextOk = false;
        for (uint32_t r = 0; r < 16; r++)
          if (!ambPatternOn((uint8_t)mask, ambShuffleNext((uint8_t)mask, (uint8_t)cur, r)))
            shufOk = false;
      }
      for (uint32_t r = 0; r < 16; r++) {
        if (!ambPatternOn((uint8_t)mask, ambFirst((uint8_t)mask, true, r)))  firstOk = false;
        if (!ambPatternOn((uint8_t)mask, ambFirst((uint8_t)mask, false, r))) firstOk = false;
      }
    }
    ck(nextOk,  "ambNextOn lands on a ticked pattern, every mask and start");
    ck(shufOk,  "ambShuffleNext lands on a ticked pattern, every mask and draw");
    ck(firstOk, "ambFirst opens on a ticked pattern, every mask");
  }

  printf("\n--- a single ticked pattern ----------------------------------\n");
  {
    bool ok = true;
    for (int p = 0; p < AMB_PATTERNS; p++) {
      const uint8_t mask = (uint8_t)(1u << p);
      // Every route must return that one pattern, and must return at all --
      // the earlier draw-and-reroll shuffle would have spun here forever.
      if (ambNextOn(mask, (uint8_t)p) != p) ok = false;
      if (ambFirst(mask, true, 12345) != p) ok = false;
      if (ambFirst(mask, false, 0) != p) ok = false;
      for (uint32_t r = 0; r < 64; r++)
        if (ambShuffleNext(mask, (uint8_t)p, r) != p) ok = false;
      for (int from = 0; from < AMB_PATTERNS; from++)
        if (ambNextOn(mask, (uint8_t)from) != p) ok = false;
    }
    ck(ok, "with one pattern ticked everything returns it and terminates");
  }

  printf("\n--- stepping in order ----------------------------------------\n");
  {
    // Long-press with everything on walks 0,1,2,3,4,0...
    uint8_t p = AMB_PAT_LIFE;
    bool ok = true;
    for (int i = 1; i <= AMB_PATTERNS; i++) {
      p = ambNextOn(AMB_PATTERN_ALL, p);
      if (p != i % AMB_PATTERNS) ok = false;
    }
    ck(ok, "all ticked: long-press cycles 0,1,2,3,4 and wraps");
  }
  {
    // Life and fireworks only: stepping must alternate between exactly those.
    const uint8_t mask = (1 << AMB_PAT_LIFE) | (1 << AMB_PAT_SPARKS);
    uint8_t p = AMB_PAT_LIFE;
    p = ambNextOn(mask, p);
    const bool a = (p == AMB_PAT_SPARKS);
    p = ambNextOn(mask, p);
    ck(a && p == AMB_PAT_LIFE, "two ticked: stepping alternates between them");
  }
  {
    // Starting from a pattern that was just unticked still moves on.
    const uint8_t mask = (1 << AMB_PAT_STARS);
    ck(ambNextOn(mask, AMB_PAT_LIFE) == AMB_PAT_STARS,
       "stepping off an unticked pattern finds the ticked one");
  }

  printf("\n--- shuffle --------------------------------------------------\n");
  {
    // With more than one ticked, shuffle must always move somewhere else --
    // repeating the current pattern is what "shuffle" exists to avoid.
    bool moved = true;
    for (int mask = 1; mask < MASKS; mask++) {
      if (ambCountOn((uint8_t)mask) < 2) continue;
      for (int cur = 0; cur < AMB_PATTERNS; cur++) {
        if (!ambPatternOn((uint8_t)mask, (uint8_t)cur)) continue;
        for (uint32_t r = 0; r < 32; r++)
          if (ambShuffleNext((uint8_t)mask, (uint8_t)cur, r) == cur) moved = false;
      }
    }
    ck(moved, "shuffle never returns the pattern already running");
  }
  {
    // And it must be able to reach every other ticked pattern, or one of them
    // would simply never appear.
    bool reachesAll = true;
    for (int mask = 1; mask < MASKS; mask++) {
      for (int cur = 0; cur < AMB_PATTERNS; cur++) {
        if (!ambPatternOn((uint8_t)mask, (uint8_t)cur)) continue;
        for (int want = 0; want < AMB_PATTERNS; want++) {
          if (want == cur || !ambPatternOn((uint8_t)mask, (uint8_t)want)) continue;
          bool hit = false;
          for (uint32_t r = 0; r < 64 && !hit; r++)
            if (ambShuffleNext((uint8_t)mask, (uint8_t)cur, r) == want) hit = true;
          if (!hit) reachesAll = false;
        }
      }
    }
    ck(reachesAll, "shuffle can reach every other ticked pattern");
  }

  printf("\n--- opening the run ------------------------------------------\n");
  {
    // The reported complaint: ambient always started on Life.
    bool varied = false;
    for (uint32_t r = 0; r < 16; r++)
      if (ambFirst(AMB_PATTERN_ALL, true, r) != AMB_PAT_LIFE) varied = true;
    ck(varied, "shuffling, a run does not always open on Life");

    bool all[AMB_PATTERNS] = {false};
    for (uint32_t r = 0; r < 64; r++) all[ambFirst(AMB_PATTERN_ALL, true, r)] = true;
    bool every = true;
    for (int p = 0; p < AMB_PATTERNS; p++) if (!all[p]) every = false;
    ck(every, "and any of them can be the one it opens on");

    ck(ambFirst(AMB_PATTERN_ALL, false, 999) == AMB_PAT_LIFE,
       "not shuffling, it opens on Life as before");
    ck(ambFirst((uint8_t)(1 << AMB_PAT_RAIN), false, 0) == AMB_PAT_RAIN,
       "unless Life is unticked, and then on the first ticked one");
  }

  printf("\n--- out of range ---------------------------------------------\n");
  {
    // A stored pattern index from a build with more patterns must not index
    // past the table.
    ck(!ambPatternOn(AMB_PATTERN_ALL, AMB_PATTERNS), "an out-of-range pattern is never on");
    ck(ambPatternOn(AMB_PATTERN_ALL, ambNextOn(AMB_PATTERN_ALL, 99)),
       "and stepping from one still lands somewhere valid");
    ck(ambPatternOn(AMB_PATTERN_ALL, ambShuffleNext(AMB_PATTERN_ALL, 99, 7)),
       "as does shuffling from one");
  }

  printf("\n--- names line up with Settings.cpp --------------------------\n");
  for (int p = 0; p < AMB_PATTERNS; p++) printf("        %d = %s\n", p, NAMES[p]);

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
