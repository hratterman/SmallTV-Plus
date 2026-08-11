#!/usr/bin/env python3
"""Render 240x240 previews of every screen that has a font face, in every face.

This is how the faces get validated without a panel on the desk: the glyph
bitmaps come from the SAME generated headers the firmware compiles
(src/NumFonts.h, src/features/clock/font_clock_sans.h), the pixel font is the
classic 5x7 read out of the Arduino_GFX library's own glcdfont.h, and the
seven-segment digits use the segment rectangles from ClockFaces.h's geometry.
What these PNGs show is what the panel draws, minus only the panel.

Output: tools/previews/*.png (gitignored; regenerate at will).
Scale: 2x, because 240px is small on a monitor.
"""
import re
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "tools" / "previews"
SCALE = 2

# ---- colours (RGB565 -> RGB) -------------------------------------------------
def c565(v):
    return ((v >> 11) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31)

WHITE = c565(0xFFFF); BLACK = (0, 0, 0); GREEN = c565(0x07E0); RED = c565(0xF800)
DGRAY = c565(0x4208); GRAY = c565(0x8410); DIMU = c565(0xB574)
PANEL = c565(0x18E3); BARBG = c565(0x2945); UGREEN = c565(0x7C6B)
BLUE = c565(0x34BF); PANEL7 = c565(0x10A2); CDIM = c565(0xB574)

# ---- the generated sans faces ------------------------------------------------
def load_gfxfonts(path):
    """Parse every GFXfont in a generated header into {name: font-dict}."""
    t = Path(path).read_text()
    fonts = {}
    for name in re.findall(r"static const GFXfont (\w+) PROGMEM", t):
        bm = [int(x, 16) for x in re.findall(
            r"0x([0-9A-F]{2}),", t.split(f"{name}Bitmaps[] PROGMEM = {{")[1].split("};")[0])]
        glyphs = []
        gsec = t.split(f"{name}Glyphs[] PROGMEM = {{")[1].split("};")[0]
        for m in re.finditer(r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\}", gsec):
            o, w, h, adv, xo, yo = (int(v) for v in m.groups())
            glyphs.append((o, w, h, adv, xo, yo))
        first = int(re.search(rf"\(uint8_t\*\){name}Bitmaps, \(GFXglyph\*\){name}Glyphs,\s*\n\s*0x([0-9A-F]+)", t).group(1), 16)
        fonts[name] = {"bitmap": bm, "glyphs": glyphs, "first": first}
    return fonts

NUM = load_gfxfonts(ROOT / "src/NumFonts.h")
CLK = load_gfxfonts(ROOT / "src/features/clock/font_clock_sans.h")
NUM_META = {  # name -> ascent (from the header defines)
    m.group(1): int(m.group(2))
    for m in re.finditer(r"\{&(NumSans\w+), (\d+), (\d+)\},",
                         (ROOT / "src/NumFonts.h").read_text())
}
CLOCK_ASC = int(re.search(r"CLOCK_SANS_ASCENT\s+(\d+)",
                          (ROOT / "src/features/clock/font_clock_sans.h").read_text()).group(1))

def sans_w(font, s):
    return sum(font["glyphs"][ord(ch) - font["first"]][3] for ch in s)

def sans_draw(px, font, x, baseline, s, color):
    """The Adafruit drawChar algorithm, bit for bit."""
    for ch in s:
        o, w, h, adv, xo, yo = font["glyphs"][ord(ch) - font["first"]]
        bit = 0
        for yy in range(h):
            for xx in range(w):
                byte = font["bitmap"][o + bit // 8]
                if byte & (0x80 >> (bit % 8)):
                    px[x + xo + xx, baseline + yo + yy] = color
                bit += 1
        x += adv
    return x

# ---- the classic 5x7 pixel font, from the library itself ----------------------
GLCD = None
for p in (ROOT / ".pio/libdeps").rglob("glcdfont.h"):
    t = p.read_text()
    GLCD = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", t)][:256 * 5]
    break
assert GLCD and len(GLCD) >= 128 * 5, "glcdfont not found in libdeps"

def px_draw(px, x, y, s, size, color, W=240, H=240):
    for ch in s:
        for col in range(5):
            bits = GLCD[ord(ch) * 5 + col]
            for row in range(8):
                if bits & (1 << row):
                    for dx in range(size):
                        for dy in range(size):
                            xx, yy = x + col * size + dx, y + row * size + dy
                            if 0 <= xx < W and 0 <= yy < H:
                                px[xx, yy] = color
        x += 6 * size
    return x

def px_w(s, size):
    return len(s) * 6 * size

def px_centered(px, y, s, size, color):
    px_draw(px, max(0, (240 - px_w(s, size)) // 2), y, s, size, color)

# ---- seven-segment, ClockFaces.h geometry -------------------------------------
SEGS = {  # segRect() transcribed; (seg letter) -> lambda(w,h,th)
    'A': lambda w, h, th, hh: (th, 0, w - 2 * th, th),
    'B': lambda w, h, th, hh: (w - th, th, th, hh),
    'C': lambda w, h, th, hh: (w - th, 2 * th + hh, th, hh),
    'D': lambda w, h, th, hh: (th, h - th, w - 2 * th, th),
    'E': lambda w, h, th, hh: (0, 2 * th + hh, th, hh),
    'F': lambda w, h, th, hh: (0, th, th, hh),
    'G': lambda w, h, th, hh: (th, th + hh, w - 2 * th, th),
}
SEG_DIGITS = ["ABCDEF", "BC", "ABDEG", "ABCDG", "BCFG", "ACDFG", "ACDEFG",
              "ABC", "ABCDEFG", "ABCDFG"]

def seg_digit(d, x, y, w, h, th, digit, on, off):
    hh = (h - 3 * th) // 2
    lit = SEG_DIGITS[digit] if 0 <= digit <= 9 else ""
    for name, fn in SEGS.items():
        rx, ry, rw, rh = fn(w, h, th, hh)
        col = on if name in lit else off
        d.rectangle([x + rx + 1, y + ry + 1, x + rx + rw - 2, y + ry + rh - 2], fill=col)

# ---- screens ------------------------------------------------------------------
def screen():
    img = Image.new("RGB", (240, 240), BLACK)
    return img, img.load(), ImageDraw.Draw(img)

def save(img, name):
    OUT.mkdir(exist_ok=True)
    img.resize((240 * SCALE, 240 * SCALE), Image.NEAREST).save(OUT / f"{name}.png")
    print(f"  {name}.png")

def num_face_for(s, max_w):
    for name in ("NumSansBig", "NumSansMid", "NumSansSmall"):
        if sans_w(NUM[name], s) <= max_w:
            return name
    return "NumSansSmall"

def ticker(face):
    img, px, d = screen()
    # page dots
    for i in range(3):
        d.ellipse([104 + i * 10, 7, 108 + i * 10, 11], fill=WHITE if i == 0 else DGRAY)
    px_centered(px, 26, "NVIDIA", 3, WHITE)
    price, change = "$182.55", "+4.20 (+2.36%)"
    y = 74
    if face == "sans":
        f = num_face_for(price, 236)
        w = sans_w(NUM[f], price)
        sans_draw(px, NUM[f], (240 - w) // 2, y + NUM_META[f], price, WHITE)
        y += NUM_META[f] + 12 + 8
    else:
        sz = 5 if px_w(price, 5) <= 236 else 4
        px_centered(px, y, price, sz, WHITE)
        y += 8 * sz + 8
    # change + arrow
    if face == "sans":
        f = num_face_for(change, 200)
        tw, ah = sans_w(NUM[f], change), NUM_META[f]
    else:
        sz = 2
        tw, ah = px_w(change, sz), 8 * sz
    aw = ah
    x = max(2, (240 - (aw + 4 + tw)) // 2)
    d.polygon([(x, y + ah), (x + aw, y + ah), (x + aw // 2, y)], fill=GREEN)
    if face == "sans":
        sans_draw(px, NUM[f], x + aw + 4, y + NUM_META[f], change, GREEN)
        y += ah + 14
    else:
        px_draw(px, x + aw + 4, y, change, 2, GREEN)
        y += ah + 8
    # sparkline
    import math
    # The chart band sits below whatever the text used; the device computes
    # this from `y` exactly the same way.
    cy0, cy1 = max(y + 6, 150), 208
    pts = [(20 + i * 4, cy1 - int((cy1 - cy0) * (math.sin(i / 5) + 1) / 2.2)) for i in range(50)]
    d.line(pts, fill=GREEN, width=2)
    px_draw(px, 208, cy0 - 10, "3M", 1, GRAY)
    save(img, f"ticker_{face}")

def usage(face):
    img, px, d = screen()
    # tiny mascot stand-in + title
    d.rectangle([6, 4, 45, 43], fill=c565(0xCBED))
    px_draw(px, 56, 12, "CLAUDE", 3, WHITE)
    for top, label, pct, reset in ((50, "5h", 42, "in 2h 05m"), (138, "7d", 87, "in 3d 4h")):
        d.rounded_rectangle([8, top, 231, top + 81], 8, fill=PANEL)
        pc = f"{pct}%"
        if face == "sans":
            f = num_face_for(pc, 150)
            if NUM_META[f] + 2 > 43:
                f = "NumSansMid"
            sans_draw(px, NUM[f], 22, top + 9 + NUM_META[f], pc, WHITE)
        else:
            px_draw(px, 22, top + 10, pc, 5, WHITE)
        px_draw(px, 231 - 14 - px_w(label, 2), top + 12, label, 2, DIMU)
        d.rounded_rectangle([22, top + 52, 209, top + 63], 6, fill=BARBG)
        fw = int(187 * pct / 100)
        color = UGREEN if pct < 75 else c565(0xDBAA)
        d.rounded_rectangle([22, top + 52, 22 + fw, top + 63], 6, fill=color)
        px_draw(px, 22, top + 64, f"Resets {reset}", 2, DIMU)
    save(img, f"usage_{face}")

def miner(face):
    img, px, d = screen()
    px_draw(px, 10, 14, "MINER", 2, c565(0xFD20))
    px_draw(px, 158, 14, "mining", 1, GREEN)
    d.rounded_rectangle([8, 34, 231, 110], 8, fill=PANEL)
    num, unit = "78.4", "kH/s"
    RATE_Y, RATE_H = 34, 76
    if face == "sans":
        f = num_face_for(num, 150)
        nw, uw = sans_w(NUM[f], num), px_w(unit, 2)
        x0 = (240 - (nw + 8 + uw)) // 2
        ny = RATE_Y + (RATE_H - (NUM_META[f] + 12)) // 2
        sans_draw(px, NUM[f], x0, ny + NUM_META[f], num, WHITE)
        px_draw(px, x0 + nw + 8, ny + NUM_META[f] - 16, unit, 2, DIMU)
    else:
        nsz = 5
        nw, uw = px_w(num, nsz), px_w(unit, 2)
        x0 = (240 - (nw + 8 + uw)) // 2
        ny = RATE_Y + (RATE_H - 8 * nsz) // 2
        px_draw(px, x0, ny, num, nsz, WHITE)
        px_draw(px, x0 + nw + 8, ny + 8 * nsz - 16, unit, 2, DIMU)
    for i, row in enumerate(("shares 3/28", "best 0.041", "jobs 112", "up 4h 12m")):
        px_draw(px, 20, 124 + i * 20, row, 2, GRAY if i else WHITE)
    px_centered(px, 214, "solo.ckpool.org  4h 12m", 1, CDIM)
    save(img, f"miner_{face}")

def clock(face):
    img, px, d = screen()
    hhmm, suffix = "12:34", "PM"
    centerY = 78
    if face == "sans":
        meta = re.search(r"CLOCK_SANS_DIGIT_ADV (\d+)",
                         (ROOT / "src/features/clock/font_clock_sans.h").read_text())
        dadv = int(meta.group(1))
        cadv = int(re.search(r"CLOCK_SANS_COLON_ADV (\d+)",
                             (ROOT / "src/features/clock/font_clock_sans.h").read_text()).group(1))
        w = sum(cadv if c == ':' else dadv for c in hhmm)
        sw = px_w(suffix, 2) + 6
        x = (240 - (w + sw)) // 2
        baseline = centerY + CLOCK_ASC // 2
        sans_draw(px, CLK["ClockSans"], x, baseline, hhmm, WHITE)
        px_draw(px, x + w + 6, baseline - 16, suffix, 2, DIMU)
    elif face == "7seg":
        th, gap, h = 9, 8, 92
        colon_w = th + 4
        sw = px_w(suffix, 2) + 6
        cw = min(48, (240 - 8 - colon_w - 4 * gap - sw) // 4)
        total = 4 * cw + colon_w + 4 * gap
        x = (240 - (total + sw)) // 2
        y = centerY - h // 2
        digits = [1, 2, 3, 4]
        for i, dgt in enumerate(digits):
            seg_digit(d, x, y, cw, h, th, dgt, WHITE, PANEL7)
            x += cw + gap
            if i == 1:
                d.rectangle([x + 2, y + h // 3 - th // 2, x + th - 1, y + h // 3 + th // 2 - 3], fill=WHITE)
                d.rectangle([x + 2, y + 2 * h // 3 - th // 2, x + th - 1, y + 2 * h // 3 + th // 2 - 3], fill=WHITE)
                x += colon_w + gap
        px_draw(px, x + 6 - gap, y + h - 16, suffix, 2, DIMU)
    else:
        size = 6
        w = px_w(hhmm, size)
        sw = px_w(suffix, 2) + 6
        x = (240 - (w + sw)) // 2
        y = centerY - 8 * size // 2
        px_draw(px, x, y, hhmm, size, WHITE)
        px_draw(px, x + w + 6, y + 8 * size - 16, suffix, 2, DIMU)
    # seconds bar + date + tz
    d.rounded_rectangle([20, 128, 219, 133], 3, fill=PANEL)
    d.rounded_rectangle([20, 128, 20 + 120, 133], 3, fill=BLUE)
    px_centered(px, 160, "Monday 10 Aug", 2, DIMU)
    px_centered(px, 214, "America/Denver", 1, DGRAY)
    save(img, f"clock_{face}")

if __name__ == "__main__":
    print("rendering previews:")
    for f in ("pixel", "sans"):
        ticker(f)
        usage(f)
        miner(f)
    for f in ("pixel", "sans", "7seg"):
        clock(f)
