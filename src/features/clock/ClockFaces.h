// ClockFaces.h — the clock's faces that are drawn rather than typeset.
//
// The seven-segment tables live here, pure, so the digit shapes can be checked
// on a host: a wrong bit in this table is a clock that reads 5 when it is 6,
// which is worse than a crash because nothing reports it.
//
// Segment names follow the datasheet convention:
//        A
//       ---
//    F |   | B
//       -G-
//    E |   | C
//       ---
//        D
#pragma once
#include <stdint.h>

#define SEG_A 0x01
#define SEG_B 0x02
#define SEG_C 0x04
#define SEG_D 0x08
#define SEG_E 0x10
#define SEG_F 0x20
#define SEG_G 0x40

// The classic map, digit -> lit segments.
static const uint8_t kSevenSeg[10] = {
    /*0*/ SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    /*1*/ SEG_B | SEG_C,
    /*2*/ SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,
    /*3*/ SEG_A | SEG_B | SEG_C | SEG_D | SEG_G,
    /*4*/ SEG_B | SEG_C | SEG_F | SEG_G,
    /*5*/ SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,
    /*6*/ SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    /*7*/ SEG_A | SEG_B | SEG_C,
    /*8*/ SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    /*9*/ SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,
};

// One segment's rectangle within a w x h digit cell whose stroke is th px.
// Returns false for a segment index outside A..G. Pure, so the same geometry
// the panel draws is the geometry the host test measures.
struct SegRect { int x, y, w, h; };

static inline bool segRect(uint8_t seg, int w, int h, int th, SegRect& r) {
  const int hh = (h - 3 * th) / 2;      // the two vertical runs
  switch (seg) {
    case SEG_A: r = {th, 0, w - 2 * th, th}; return true;
    case SEG_B: r = {w - th, th, th, hh}; return true;
    case SEG_C: r = {w - th, 2 * th + hh, th, hh}; return true;
    case SEG_D: r = {th, h - th, w - 2 * th, th}; return true;
    case SEG_E: r = {0, 2 * th + hh, th, hh}; return true;
    case SEG_F: r = {0, th, th, hh}; return true;
    case SEG_G: r = {th, th + hh, w - 2 * th, th}; return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Analog and flip face geometry — pure, shared by the panel, the host test
// and the preview renderer, so what is measured is what is drawn.
// ---------------------------------------------------------------------------
struct AnalogGeom { int cx, cy, r; };
static inline AnalogGeom clockAnalogGeom(int W, int centerY) {
  // r chosen so the dial bottom stays above the seconds bar at y=128.
  return {W / 2, centerY, 48};
}
// Hand angles in degrees, 0 at 3 o'clock, screen-clockwise positive.
static inline float clockHourAngleDeg(int h, int m) {
  return (h % 12) * 30.0f + m * 0.5f - 90.0f;
}
static inline float clockMinAngleDeg(int m) { return m * 6.0f - 90.0f; }

struct FlipGeom { int xHH, xMM, y, w, h; };
static inline FlipGeom clockFlipGeom(int W, int centerY) {
  const int w = 104, h = 96, gap = 12;
  const int x0 = (W - (2 * w + gap)) / 2;
  return {x0, x0 + w + gap, centerY - h / 2, w, h};
}
