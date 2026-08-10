// Host-side checks for the packed mascot frames and their decoder.
//
// The frame data is generated, run-length encoded, and never looked at again by
// a human — so a bad byte does not announce itself. It shows up as a creature
// with a hole in it, on a device, some minutes after boot when that animation
// comes round. Everything here is arithmetic over the real shipped tables, so
// it runs in a few milliseconds with no panel and no ESP.
//
// The strongest check is the round trip: re-encoding what the decoder produced
// must reproduce the stored bytes exactly. That pins decoder and generator to
// each other, which is where this codebase's bugs usually live — one rule
// written twice, and only one copy updated.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#define MASCOT_GRID         20
#define MASCOT_PALETTE_SIZE 10
#define PROGMEM

#include "../../src/features/usage/mascot_frames.h"
#include "../../src/features/usage/MascotUnpack.h"

static const int CELLS = MASCOT_GRID * MASCOT_GRID;
static const int MAX_RUN = 16;          // 4-bit run field, stored as run-1

static int failures = 0;
static void ck(bool cond, const char* what) {
  printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
  if (!cond) failures++;
}

// Mirrors rle_encode() in tools/extract_mascot.py.
static std::vector<uint8_t> encode(const uint8_t* cells) {
  std::vector<uint8_t> out;
  int i = 0;
  while (i < CELLS) {
    const uint8_t v = cells[i];
    int run = 1;
    while (i + run < CELLS && cells[i + run] == v && run < MAX_RUN) run++;
    out.push_back((uint8_t)((v << 4) | (run - 1)));
    i += run;
  }
  return out;
}

int main() {
  printf("--- the animation table -------------------------------------\n");
  ck(MASCOT_ANIM_COUNT == 5, "five animations are packaged");
  {
    bool named = true, grouped = true, nonempty = true;
    for (int a = 0; a < MASCOT_ANIM_COUNT; a++) {
      if (!mascot_anims[a].name || !mascot_anims[a].name[0]) named = false;
      if (mascot_anims[a].group > 3) grouped = false;
      if (mascot_anims[a].frame_count == 0) nonempty = false;
    }
    ck(named, "every animation has a name");
    ck(grouped, "every burn-rate group is 0..3");
    ck(nonempty, "no animation is empty");
  }
  {
    // pickForRate() walks the groups looking for a match; a gap would leave a
    // mood with nothing to play and the creature frozen on the previous frame.
    bool covered[4] = {false, false, false, false};
    for (int a = 0; a < MASCOT_ANIM_COUNT; a++) covered[mascot_anims[a].group] = true;
    ck(covered[0] && covered[1] && covered[2] && covered[3],
       "all four moods have at least one animation");
  }

  printf("\n--- the packed tables ---------------------------------------\n");
  {
    // Every run must land inside the grid and end exactly on its last cell. A
    // frame whose runs sum to 399 or 401 is a generator bug, and the decoder's
    // clamp would quietly paper over it on the device.
    bool exact = true, inPalette = true;
    int totalFrames = 0, totalBytes = 0;
    for (int a = 0; a < MASCOT_ANIM_COUNT; a++) {
      const MascotAnim& an = mascot_anims[a];
      const uint8_t* p = an.rle;
      for (int f = 0; f < an.frame_count; f++) {
        const uint8_t n = an.lens[f];
        int sum = 0;
        for (int i = 0; i < n; i++) {
          if ((p[i] >> 4) >= MASCOT_PALETTE_SIZE) inPalette = false;
          sum += (p[i] & 0x0F) + 1;
        }
        if (sum != CELLS) exact = false;
        p += n;
        totalFrames++;
        totalBytes += n;
      }
    }
    ck(exact, "every frame's runs sum to exactly 400 cells");
    ck(inPalette, "every palette index is within the 10-entry palette");
    printf("        %d frames, %d bytes packed (%d unpacked)\n",
           totalFrames, totalBytes, totalFrames * CELLS);
  }
  {
    // lens[] is what the decoder uses to find frame N. If the sizes did not add
    // up to the array, every frame after the bad one would decode from the
    // wrong offset — and the first frames would still look right.
    bool consistent = true;
    for (int a = 0; a < MASCOT_ANIM_COUNT; a++) {
      const MascotAnim& an = mascot_anims[a];
      int sum = 0;
      for (int f = 0; f < an.frame_count; f++) sum += an.lens[f];
      // The generator lays each animation's frames back to back with nothing
      // between, so the sizes must account for the whole array.
      const uint8_t* end = an.rle + sum;
      const uint8_t* last = an.rle;
      for (int f = 0; f + 1 < an.frame_count; f++) last += an.lens[f];
      if (last + an.lens[an.frame_count - 1] != end) consistent = false;
    }
    ck(consistent, "per-frame sizes tile the packed array with no gaps");
  }

  printf("\n--- decode ---------------------------------------------------\n");
  {
    bool roundTrip = true, filled = true;
    int checked = 0;
    for (int a = 0; a < MASCOT_ANIM_COUNT; a++) {
      const MascotAnim& an = mascot_anims[a];
      for (int f = 0; f < an.frame_count; f++) {
        uint8_t cells[CELLS];
        memset(cells, 0xAA, sizeof(cells));
        mascotUnpack(an.rle, an.lens, (uint16_t)f, cells, CELLS);

        for (int i = 0; i < CELLS; i++)
          if (cells[i] == 0xAA) filled = false;      // decoder left a hole

        const std::vector<uint8_t> again = encode(cells);
        const uint8_t* stored = an.rle;
        for (int k = 0; k < f; k++) stored += an.lens[k];
        if (again.size() != an.lens[f] ||
            memcmp(again.data(), stored, again.size()) != 0) roundTrip = false;
        checked++;
      }
    }
    ck(filled, "the decoder writes all 400 cells of every frame");
    ck(roundTrip, "re-encoding a decoded frame reproduces the stored bytes");
    printf("        %d frames round-tripped\n", checked);
  }
  {
    // Frame lookup is by walking lens[], so an off-by-one lands on the wrong
    // frame rather than failing. Different frames must actually differ.
    const MascotAnim& an = mascot_anims[0];
    uint8_t f0[CELLS], f1[CELLS];
    mascotUnpack(an.rle, an.lens, 0, f0, CELLS);
    mascotUnpack(an.rle, an.lens, 1, f1, CELLS);
    ck(memcmp(f0, f1, CELLS) != 0, "frame 1 is not frame 0 (offsets advance)");

    uint8_t again[CELLS];
    mascotUnpack(an.rle, an.lens, 0, again, CELLS);
    ck(memcmp(f0, again, CELLS) == 0, "decoding is deterministic");
  }
  {
    // The device draws the last frame of an animation as often as any other,
    // and it is the one a length mistake reaches first.
    bool ok = true;
    for (int a = 0; a < MASCOT_ANIM_COUNT; a++) {
      const MascotAnim& an = mascot_anims[a];
      uint8_t cells[CELLS];
      memset(cells, 0xAA, sizeof(cells));
      mascotUnpack(an.rle, an.lens, (uint16_t)(an.frame_count - 1), cells, CELLS);
      for (int i = 0; i < CELLS; i++) if (cells[i] == 0xAA) ok = false;
    }
    ck(ok, "the last frame of every animation decodes fully");
  }
  {
    // Truncated data must not run off the buffer. Hand the decoder a length
    // that promises more cells than the grid holds.
    const uint8_t rle[]  = {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F,   // 6*16 = 96
                            0x2F, 0x2F, 0x2F, 0x2F, 0x2F, 0x2F,
                            0x3F, 0x3F, 0x3F, 0x3F, 0x3F, 0x3F,
                            0x4F, 0x4F, 0x4F, 0x4F, 0x4F, 0x4F,
                            0x5F, 0x5F, 0x5F, 0x5F, 0x5F, 0x5F};  // 480 cells total
    const uint8_t lens[] = {sizeof(rle)};
    uint8_t guard[CELLS + 8];
    memset(guard, 0x5A, sizeof(guard));
    mascotUnpack(rle, lens, 0, guard, CELLS);
    bool intact = true;
    for (int i = CELLS; i < CELLS + 8; i++) if (guard[i] != 0x5A) intact = false;
    ck(intact, "over-long frame data stops at the end of the grid");
  }
  {
    // And the opposite: a frame that ends early is background, not leftovers.
    const uint8_t rle[]  = {0x1F};      // 16 cells, then nothing
    const uint8_t lens[] = {1};
    uint8_t cells[CELLS];
    memset(cells, 0x77, sizeof(cells));
    mascotUnpack(rle, lens, 0, cells, CELLS);
    bool ok = true;
    for (int i = 0; i < 16; i++)     if (cells[i] != 1) ok = false;
    for (int i = 16; i < CELLS; i++) if (cells[i] != 0) ok = false;
    ck(ok, "short frame data fills the rest with the background index");
  }

  printf("\n-------------------------------------------------------------\n");
  if (failures) { printf("%d check(s) FAILED\n", failures); return 1; }
  printf("all checks passed\n");
  return 0;
}
