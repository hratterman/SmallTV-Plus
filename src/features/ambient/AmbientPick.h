// AmbientPick.h — choosing which ambient pattern runs next.
//
// Pure arithmetic over the ticked-pattern mask, kept out of AmbientMode.cpp so
// it can be run on a host. The failure modes here are not subtle bugs so much
// as bad ones: showing a pattern the user unticked, or spinning forever looking
// for a different one when only a single pattern is ticked. The randomness is
// passed in rather than drawn here, so every case is deterministic to test.
#pragma once
#include <stdint.h>
#include "config.h"        // AMB_PAT_*, AMB_PATTERNS, AMB_PATTERN_ALL

// An empty mask is read as "all of them". Settings already refuses to store
// one, and this refuses it again: a hand-edited config.json reaches the same
// code, and the worst thing it could do is leave a black screen saying nothing.
static inline uint8_t ambMaskOrAll(uint8_t mask) {
  const uint8_t m = mask & AMB_PATTERN_ALL;
  return m ? m : AMB_PATTERN_ALL;
}

static inline bool ambPatternOn(uint8_t mask, uint8_t pat) {
  return pat < AMB_PATTERNS && (ambMaskOrAll(mask) & (uint8_t)(1u << pat)) != 0;
}

static inline uint8_t ambCountOn(uint8_t mask) {
  const uint8_t m = ambMaskOrAll(mask);
  uint8_t n = 0;
  for (uint8_t p = 0; p < AMB_PATTERNS; p++) if (m & (1u << p)) n++;
  return n;
}

// The next ticked pattern after `from`, wrapping. Returns `from` when it is the
// only one ticked — the caller wants a pattern to run, not a failure.
static inline uint8_t ambNextOn(uint8_t mask, uint8_t from) {
  const uint8_t m = ambMaskOrAll(mask);
  if (from >= AMB_PATTERNS) from = 0;
  for (uint8_t i = 1; i <= AMB_PATTERNS; i++) {
    const uint8_t p = (uint8_t)((from + i) % AMB_PATTERNS);
    if (m & (1u << p)) return p;
  }
  return from;
}

// A ticked pattern other than `cur`, chosen by `r`. Falls back to ambNextOn
// when `cur` is the only ticked one, so this always terminates.
//
// Counts the candidates and indexes into them rather than drawing and
// re-rolling: re-rolling takes unboundedly long as the ticked set shrinks, and
// never finishes at all when only one is ticked.
static inline uint8_t ambShuffleNext(uint8_t mask, uint8_t cur, uint32_t r) {
  const uint8_t m = ambMaskOrAll(mask);
  if (cur >= AMB_PATTERNS) cur = 0;

  uint8_t others = 0;
  for (uint8_t p = 0; p < AMB_PATTERNS; p++)
    if (p != cur && (m & (1u << p))) others++;
  if (!others) return ambNextOn(m, cur);

  uint8_t k = (uint8_t)(r % others);
  for (uint8_t p = 0; p < AMB_PATTERNS; p++) {
    if (p == cur || !(m & (1u << p))) continue;
    if (k == 0) return p;
    k--;
  }
  return ambNextOn(m, cur);        // unreachable
}

// Where a run opens. Honours the same shuffle switch as the advance, so the
// screen does not always come up on Life, and never opens on an unticked one.
static inline uint8_t ambFirst(uint8_t mask, bool shuffle, uint32_t r) {
  const uint8_t m = ambMaskOrAll(mask);
  if (!shuffle) return ambPatternOn(m, AMB_PAT_LIFE) ? (uint8_t)AMB_PAT_LIFE
                                                     : ambNextOn(m, AMB_PAT_LIFE);
  uint8_t k = (uint8_t)(r % ambCountOn(m));
  for (uint8_t p = 0; p < AMB_PATTERNS; p++) {
    if (!(m & (1u << p))) continue;
    if (k == 0) return p;
    k--;
  }
  return AMB_PAT_LIFE;             // unreachable
}
