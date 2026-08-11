#!/usr/bin/env python3
"""Rasterise TTF subsets into the Adafruit-GFX font headers the firmware draws.

Two headers come out of here:

  src/features/clock/font_clock_sans.h — the clock's big Sans face. Eleven
  glyphs ('0'..'9' and ':') at display size, digits forced monospaced so the
  time does not shuffle sideways as the minutes tick.

  src/NumFonts.h — the device-wide "numbers" face, in three sizes (big, mid,
  small) so call sites can auto-fit the way they already do with the pixel
  font. Charset is the contiguous run 0x20..0x3A, which is exactly the
  characters a price, a percentage, a hashrate or a change line can contain:
  $ % & ' ( ) * + , - . / 0-9 :  — letters live in the text faces below.
  Digits are monospaced per size; punctuation keeps natural width.

  src/TextFonts.h — the labels-and-letters faces, full printable ASCII in the
  two sizes the UI actually uses for static text (pixel sizes 3 and 2). Each
  face is auto-sized DOWN until every glyph fits inside the corresponding
  pixel band (24 px and 16 px total): a call site that fit the pixel font is
  then geometrically incapable of clipping in sans, which is what lets
  gfxDrawCentered switch faces without auditing every caller. A handful of
  glyphs no label ever shows (braces, brackets, pipe...) are exempt from the
  constraint and cropped to the band instead, so they do not drag the whole
  face a point smaller.

Every glyph is verified to survive a round trip through the bitmap packing
before anything is written, and the headers compile on a host (the GFXfont
structs are declared locally when Arduino_GFX is absent) so the self-tests can
read the same bytes the panel draws.

Needs Pillow. Regenerate with:  python3 tools/gen_font.py
"""
from PIL import Image, ImageFont, ImageDraw
from pathlib import Path

TTF = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"

CLOCK_PT = 76
CLOCK_CHARS = "0123456789:"
NUM_CHARS = "".join(chr(c) for c in range(0x20, 0x3B))   # space .. : (covers "+1.2 (0.4%)")
NUM_SIZES = [("Big", 54), ("Mid", 34), ("Small", 23)]    # ~38 / ~24 / ~16 px digits

TEXT_CHARS = "".join(chr(c) for c in range(0x20, 0x7F))  # all printable ASCII
TEXT_SIZES = [("Mid", 24), ("Small", 16)]                # pixel size 3 / size 2 bands
# Glyphs no on-screen label ever contains: exempt from the fit constraint and
# cropped to the band instead, so their tall extents don't shrink the face.
TALL_EXEMPT = set("|{}[]\\`~")

HOST_GUARD = """#ifdef ARDUINO
#include <Arduino_GFX_Library.h>   // GFXfont / GFXglyph
#else
// Host builds (the self-tests and the preview renderer's checker) get the
// structs directly; the layout matches Adafruit GFX exactly. Guarded so both
// generated headers can appear in one translation unit.
#include <stdint.h>
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef GFX_HOST_STRUCTS
#define GFX_HOST_STRUCTS
typedef struct {
  uint16_t bitmapOffset;
  uint8_t  width, height;
  uint8_t  xAdvance;
  int8_t   xOffset, yOffset;
} GFXglyph;
typedef struct {
  uint8_t  *bitmap;
  GFXglyph *glyph;
  uint16_t  first, last;
  uint8_t   yAdvance;
} GFXfont;
#endif
#endif"""


def render_glyph(font, ch, size_pt):
    """Tight 1-bit bitmap plus metrics, baseline-anchored."""
    W = H = size_pt * 3
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    origin = (size_pt, size_pt * 2)
    # anchor="ls": the origin is the BASELINE, matching the GFXfont convention.
    # Pillow's default anchors at the ascender, which flips every yOffset's
    # sign and draws the whole face one line low.
    d.text(origin, ch, font=font, fill=255, anchor="ls")
    bbox = img.getbbox()
    if bbox is None:
        return [], 0, 0, 0, 0
    x0, y0, x1, y1 = bbox
    img = img.crop(bbox).point(lambda v: 255 if v >= 128 else 0)
    w, h = img.size
    rows = [[1 if img.getpixel((x, y)) else 0 for x in range(w)] for y in range(h)]
    return rows, w, h, x0 - origin[0], y0 - origin[1]


def pack(rows, w, h):
    """GFXfont bitmap: rows concatenated, MSB first, no per-row padding."""
    bits = [px for row in rows for px in row]
    out = []
    for i in range(0, len(bits), 8):
        b = 0
        for j, px in enumerate(bits[i:i + 8]):
            if px:
                b |= 0x80 >> j
        out.append(b)
    return out


def unpack(data, w, h):
    bits = []
    for b in data:
        for j in range(8):
            bits.append(1 if b & (0x80 >> j) else 0)
    return [bits[y * w:(y + 1) * w] for y in range(h)]


def rasterise(size_pt, chars):
    font = ImageFont.truetype(TTF, size_pt)
    glyphs = []
    for ch in chars:
        rows, w, h, xo, yo = render_glyph(font, ch, size_pt)
        glyphs.append({"ch": ch, "rows": rows, "w": w, "h": h,
                       "xo": xo, "yo": yo, "adv": round(font.getlength(ch))})
    return glyphs


def finish_font(name, glyphs):
    """Monospace the digits, pack, verify. Returns (entries, bitmap, asc, desc, digit_adv)."""
    digit_adv = max(g["adv"] for g in glyphs if g["ch"].isdigit())
    for g in glyphs:
        if g["ch"].isdigit():
            g["xo"] += (digit_adv - g["adv"]) // 2
            g["adv"] = digit_adv

    bitmap, entries = [], []
    for g in glyphs:
        data = pack(g["rows"], g["w"], g["h"])
        if g["rows"] and unpack(data, g["w"], g["h"]) != [r[:] for r in g["rows"]]:
            raise SystemExit(f"{name}: glyph '{g['ch']}' fails the round trip")
        entries.append((len(bitmap), g))
        bitmap.extend(data)

    ascent = max((-g["yo"] for g in glyphs if g["rows"]), default=0)
    descent = max((g["yo"] + g["h"] for g in glyphs if g["rows"]), default=0)
    return entries, bitmap, ascent, descent, digit_adv


def build_font(name, size_pt, chars):
    """Returns (glyph entries, bitmap bytes, ascent, descent, digit_adv)."""
    return finish_font(name, rasterise(size_pt, chars))


def build_text_font(name, band, chars):
    """A text face auto-sized DOWN until every non-exempt glyph fits `band` px
    of total height. The exempt glyphs are then cropped into that band, so the
    face as a whole cannot draw outside it — the property the firmware's
    switch-inside-gfxDrawCentered relies on."""
    for pt in range(band + 6, 6, -1):
        glyphs = rasterise(pt, chars)
        core = [g for g in glyphs if g["ch"] not in TALL_EXEMPT and g["rows"]]
        asc = max(-g["yo"] for g in core)
        desc = max(g["yo"] + g["h"] for g in core)
        if asc + desc <= band:
            break
    else:
        raise SystemExit(f"{name}: no point size fits a {band} px band")

    for g in glyphs:
        if g["ch"] not in TALL_EXEMPT or not g["rows"]:
            continue
        over_top = -g["yo"] - asc
        if over_top > 0:
            g["rows"] = g["rows"][over_top:]
            g["h"] -= over_top
            g["yo"] += over_top
        over_bot = (g["yo"] + g["h"]) - desc
        if over_bot > 0:
            g["rows"] = g["rows"][:-over_bot]
            g["h"] -= over_bot

    return finish_font(name, glyphs) + (pt,)


def emit_font(name, chars, entries, bitmap, ascent, descent):
    lines = [f"static const uint8_t {name}Bitmaps[] PROGMEM = {{"]
    for i in range(0, len(bitmap), 16):
        lines.append("  " + "".join(f"0x{b:02X}," for b in bitmap[i:i + 16]))
    lines += ["};", "", f"static const GFXglyph {name}Glyphs[] PROGMEM = {{"]
    for off, g in entries:
        lines.append(f"  {{{off:5d}, {g['w']:3d}, {g['h']:3d}, {g['adv']:3d}, "
                     f"{g['xo']:3d}, {g['yo']:4d}}},   // '{g['ch']}'")
    lines += [
        "};", "",
        f"static const GFXfont {name} PROGMEM = {{",
        f"  (uint8_t*){name}Bitmaps, (GFXglyph*){name}Glyphs,",
        f"  0x{ord(chars[0]):02X}, 0x{ord(chars[-1]):02X}, {ascent + descent + 6}}};",
        "",
    ]
    return lines


def main():
    # ---- the clock face -----------------------------------------------------
    entries, bitmap, asc, desc, dadv = build_font("ClockSans", CLOCK_PT, CLOCK_CHARS)
    colon_adv = next(g["adv"] for _, g in entries if g["ch"] == ":")
    out = [
        "// font_clock_sans.h - GENERATED by tools/gen_font.py. Do not edit by hand.",
        f"// {Path(TTF).name} at {CLOCK_PT}pt, glyphs '{CLOCK_CHARS}' only, digits monospaced.",
        "#pragma once",
        HOST_GUARD,
        "",
        f"#define CLOCK_SANS_ASCENT  {asc}",
        f"#define CLOCK_SANS_DESCENT {desc}",
        f"#define CLOCK_SANS_DIGIT_ADV {dadv}",
        f"#define CLOCK_SANS_COLON_ADV {colon_adv}",
        "",
    ] + emit_font("ClockSans", CLOCK_CHARS, entries, bitmap, asc, desc)
    Path("src/features/clock/font_clock_sans.h").write_text("\n".join(out))
    print(f"font_clock_sans.h: {len(bitmap)} bytes, ascent {asc}")

    # ---- the numbers faces --------------------------------------------------
    out = [
        "// NumFonts.h - GENERATED by tools/gen_font.py. Do not edit by hand.",
        f"// {Path(TTF).name}; charset 0x20..0x3A (space..'()*+,-./0-9:), digits monospaced.",
        "// Three sizes so call sites can auto-fit like they do with the pixel font.",
        "#pragma once",
        HOST_GUARD,
        "",
        f"#define NUM_FONT_FIRST 0x{ord(NUM_CHARS[0]):02X}",
        f"#define NUM_FONT_LAST  0x{ord(NUM_CHARS[-1]):02X}",
        "#define NUM_FONT_COUNT 3   // big, mid, small",
        "",
    ]
    meta = []
    total = 0
    for label, pt in NUM_SIZES:
        name = f"NumSans{label}"
        entries, bitmap, asc, desc, dadv = build_font(name, pt, NUM_CHARS)
        total += len(bitmap)
        out += emit_font(name, NUM_CHARS, entries, bitmap, asc, desc)
        meta.append((name, asc, desc, dadv))
        print(f"{name}: {len(bitmap)} bytes, ascent {asc}, digit advance {dadv}")

    out += ["// Ascent/descent per face, index big=0 mid=1 small=2 — the draw",
            "// helper turns a band's top edge into a baseline with these.",
            "typedef struct { const GFXfont* font; uint8_t ascent, descent; } NumFace;",
            "static const NumFace kNumFaces[NUM_FONT_COUNT] = {"]
    for name, asc, desc, dadv in meta:
        out.append(f"  {{&{name}, {asc}, {desc}}},")
    out += ["};", ""]
    Path("src/NumFonts.h").write_text("\n".join(out))
    print(f"NumFonts.h: {total} bytes of bitmaps across {len(NUM_SIZES)} sizes")

    # ---- the text faces -----------------------------------------------------
    out = [
        "// TextFonts.h - GENERATED by tools/gen_font.py. Do not edit by hand.",
        f"// {Path(TTF).name}; full printable ASCII, digits monospaced.",
        "// Two faces sized so that ascent+descent fits the pixel size-3 (24 px)",
        "// and size-2 (16 px) bands exactly — any call site that fit the pixel",
        "// font cannot clip in sans. Index 0 stands in for pixel size 3, 1 for 2.",
        "#pragma once",
        HOST_GUARD,
        "",
        f"#define TEXT_FONT_FIRST 0x{ord(TEXT_CHARS[0]):02X}",
        f"#define TEXT_FONT_LAST  0x{ord(TEXT_CHARS[-1]):02X}",
        "#define TEXT_FONT_COUNT 2   // pixel size 3 and size 2 stand-ins",
        "",
    ]
    tmeta = []
    total = 0
    for (label, band) in TEXT_SIZES:
        name = f"TextSans{label}"
        entries, bitmap, asc, desc, dadv, pt = build_text_font(name, band, TEXT_CHARS)
        if asc + desc > band:
            raise SystemExit(f"{name}: {asc}+{desc} exceeds its {band} px band")
        total += len(bitmap)
        out += emit_font(name, TEXT_CHARS, entries, bitmap, asc, desc)
        tmeta.append((name, asc, desc, band))
        print(f"{name}: {len(bitmap)} bytes at {pt}pt, ascent {asc} descent {desc}"
              f" (band {band}), digit advance {dadv}")

    out += ["typedef struct { const GFXfont* font; uint8_t ascent, descent, band; } TextFace;",
            "static const TextFace kTextFaces[TEXT_FONT_COUNT] = {"]
    for name, asc, desc, band in tmeta:
        out.append(f"  {{&{name}, {asc}, {desc}, {band}}},")
    out += ["};", ""]
    Path("src/TextFonts.h").write_text("\n".join(out))
    print(f"TextFonts.h: {total} bytes of bitmaps across {len(TEXT_SIZES)} sizes")


if __name__ == "__main__":
    main()
