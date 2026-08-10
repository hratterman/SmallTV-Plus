// MascotUnpack.h — turning a packed mascot frame back into a grid of cells.
//
// Split out of Mascot.cpp so it can be exercised on a host, where a wrong run
// length is a failing assert instead of a creature with a hole in it. The
// packing is described in mascot_frames.h: one byte per run, palette index in
// the high nibble, (run length - 1) in the low.
#pragma once
#include <stdint.h>

#ifdef ARDUINO
#include <pgmspace.h>
#else
#define pgm_read_byte(a) (*(const uint8_t*)(a))
#endif

// Expand frame `frame` of an animation into `out`, which must hold `cells`
// bytes. `rle` is the animation's packed frames laid back to back and `lens`
// their sizes, both in flash.
//
// Defensive on both ends deliberately: this walks flash data that a regenerated
// header could get wrong, and the alternative to clamping is scribbling past a
// 400-byte buffer. A frame that runs short is filled with the background index
// rather than left holding whatever the last frame drew.
static inline void mascotUnpack(const uint8_t* rle, const uint8_t* lens,
                                uint16_t frame, uint8_t* out, uint16_t cells) {
  const uint8_t* p = rle;
  for (uint16_t f = 0; f < frame; f++) p += pgm_read_byte(&lens[f]);

  const uint8_t n = pgm_read_byte(&lens[frame]);
  uint16_t w = 0;
  for (uint8_t i = 0; i < n && w < cells; i++) {
    const uint8_t b   = pgm_read_byte(p + i);
    const uint8_t val = (uint8_t)(b >> 4);
    for (uint8_t run = (uint8_t)((b & 0x0F) + 1); run && w < cells; run--) out[w++] = val;
  }
  while (w < cells) out[w++] = 0;
}
