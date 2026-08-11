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
TXT = load_gfxfonts(ROOT / "src/TextFonts.h")
CLK = load_gfxfonts(ROOT / "src/features/clock/font_clock_sans.h")
NUM_META = {  # name -> ascent (from the header defines)
    m.group(1): int(m.group(2))
    for m in re.finditer(r"\{&(NumSans\w+), (\d+), (\d+)\},",
                         (ROOT / "src/NumFonts.h").read_text())
}
CLOCK_ASC = int(re.search(r"CLOCK_SANS_ASCENT\s+(\d+)",
                          (ROOT / "src/features/clock/font_clock_sans.h").read_text()).group(1))
TXT_META = {  # name -> ascent, from the kTextFaces table
    m.group(1): int(m.group(2))
    for m in re.finditer(r"\{&(TextSans\w+), (\d+), (\d+), (\d+)\},",
                         (ROOT / "src/TextFonts.h").read_text())
}

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

# ---- the typeface-aware label layer (mirrors gfxLabel/gfxLabelW) --------------
def text_face_for(size):
    return {3: "TextSansMid", 2: "TextSansSmall"}.get(size)

def label_w(s, size, sans):
    f = text_face_for(size) if sans else None
    return sans_w(TXT[f], s) if f else px_w(s, size)

def label_asc(size, sans):
    f = text_face_for(size) if sans else None
    return TXT_META[f] if f else 8 * size

def label(px, x, top_y, s, size, color, sans):
    f = text_face_for(size) if sans else None
    if f and x + sans_w(TXT[f], s) <= 240:
        sans_draw(px, TXT[f], x, top_y + TXT_META[f], s, color)
    else:
        px_draw(px, x, top_y, s, size, color)

def label_centered(px, y, s, size, color, sans):
    label(px, max(0, (240 - label_w(s, size, sans)) // 2), y, s, size, color, sans)

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
    sans = face == "sans"
    label_centered(px, 26, "NVIDIA", 3, WHITE, sans)
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
    label(px, 240 - label_w("3M", 2, sans) - 4, 4, "3M", 2, GRAY, sans)
    save(img, f"ticker_{face}")

def usage(face):
    img, px, d = screen()
    # tiny mascot stand-in + title
    d.rectangle([6, 4, 45, 43], fill=c565(0xCBED))
    sans = face == "sans"
    label(px, 56, 12, "CLAUDE", 3, WHITE, sans)
    for top, lab, pct, reset in ((50, "5h", 42, "in 2h 05m"), (138, "7d", 87, "in 3d 4h")):
        d.rounded_rectangle([8, top, 231, top + 81], 8, fill=PANEL)
        pc = f"{pct}%"
        if face == "sans":
            f = num_face_for(pc, 150)
            if NUM_META[f] + 2 > 43:
                f = "NumSansMid"
            sans_draw(px, NUM[f], 22, top + 9 + NUM_META[f], pc, WHITE)
        else:
            px_draw(px, 22, top + 10, pc, 5, WHITE)
        label(px, 231 - 14 - label_w(lab, 2, sans), top + 12, lab, 2, DIMU, sans)
        d.rounded_rectangle([22, top + 52, 209, top + 63], 6, fill=BARBG)
        fw = int(187 * pct / 100)
        color = UGREEN if pct < 75 else c565(0xDBAA)
        d.rounded_rectangle([22, top + 52, 22 + fw, top + 63], 6, fill=color)
        label(px, 22, top + 64, f"Resets {reset}", 2, DIMU, sans)
    save(img, f"usage_{face}")

def miner(face):
    img, px, d = screen()
    sans = face == "sans"
    label(px, 10, 10, "MINER", 2, c565(0xFD20), sans)
    px_draw(px, 230 - px_w("mining", 1), 14, "mining", 1, GREEN)   # state pill, size 1
    # MinerMode geometry: RATE panel then the four labelled stat rows.
    RATE_Y, RATE_H, STAT_Y, STAT_H = 34, 68, 110, 92
    ROW0_Y, ROW_DY, VAL_R = 120, 22, 220
    d.rounded_rectangle([8, RATE_Y, 231, RATE_Y + RATE_H], 8, fill=PANEL)
    d.rounded_rectangle([8, STAT_Y, 231, STAT_Y + STAT_H], 8, fill=PANEL)
    num, unit = "78.4", "kH/s"
    if sans:
        f = num_face_for(num, 150)
        nw, uw = sans_w(NUM[f], num), label_w(unit, 2, sans)
        x0 = (240 - (nw + 8 + uw)) // 2
        ny = RATE_Y + (RATE_H - (NUM_META[f] + 12)) // 2
        sans_draw(px, NUM[f], x0, ny + NUM_META[f], num, WHITE)
        label(px, x0 + nw + 8, ny + NUM_META[f] - label_asc(2, sans), unit, 2, DIMU, sans)
    else:
        nsz = 5
        nw, uw = px_w(num, nsz), px_w(unit, 2)
        x0 = (240 - (nw + 8 + uw)) // 2
        ny = RATE_Y + (RATE_H - 8 * nsz) // 2
        px_draw(px, x0, ny, num, nsz, WHITE)
        px_draw(px, x0 + nw + 8, ny + 8 * nsz - 16, unit, 2, DIMU)
    rows = (("shares", "3/28", WHITE), ("best", "0.041", c565(0xFD20)),
            ("pool diff", "16.4K", DIMU), ("jobs", "112", WHITE))
    for i, (lab, val, col) in enumerate(rows):
        px_draw(px, 20, ROW0_Y + i * ROW_DY + 4, lab, 1, DIMU)
        label(px, VAL_R - label_w(val, 2, sans), ROW0_Y + i * ROW_DY, val, 2, col, sans)
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
    elif face == "analog":
        import math
        cx, cy, r = 120, centerY, 48        # clockAnalogGeom(240, 78)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=DIMU, width=2)
        for i in range(12):
            a = math.radians(i * 30)
            cardinal = i % 3 == 0
            o, inn = r - 4, r - (12 if cardinal else 8)
            col = WHITE if cardinal else DIMU
            d.line([cx + math.cos(a) * o, cy + math.sin(a) * o,
                    cx + math.cos(a) * inn, cy + math.sin(a) * inn], fill=col, width=2)
        def hand(deg, ln, hw, col):
            a = math.radians(deg)
            tx, ty = cx + math.cos(a) * ln, cy + math.sin(a) * ln
            px_, py_ = -math.sin(a) * hw, math.cos(a) * hw
            d.polygon([(cx + px_, cy + py_), (cx - px_, cy - py_), (tx, ty)], fill=col)
        hand((12 % 12) * 30 + 34 * 0.5 - 90, r - 22, 3, WHITE)   # hour 12:34
        hand(34 * 6 - 90, r - 10, 2, WHITE)                      # minute
        d.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], fill=BLUE)
        px_draw(px, cx + r + 8, cy + r - 16, suffix, 2, DIMU)
    elif face == "flip":
        w_, h_, gap = 104, 96, 12           # clockFlipGeom(240, 78)
        x0 = (240 - (2 * w_ + gap)) // 2
        y0 = centerY - h_ // 2
        dadv = int(re.search(r"CLOCK_SANS_DIGIT_ADV (\d+)",
                             (ROOT / "src/features/clock/font_clock_sans.h").read_text()).group(1))
        for cx0, two in ((x0, "12"), (x0 + w_ + gap, "34")):
            d.rounded_rectangle([cx0, y0, cx0 + w_, y0 + h_], 10, fill=PANEL)
            dw = dadv * len(two)
            sans_draw(px, CLK["ClockSans"], cx0 + (w_ - dw) // 2,
                      y0 + h_ // 2 + CLOCK_ASC // 2, two, WHITE)
            d.rectangle([cx0, y0 + h_ // 2 - 1, cx0 + w_, y0 + h_ // 2], fill=BLACK)
            d.rectangle([cx0 - 1, y0 + h_ // 2 - 4, cx0 + 2, y0 + h_ // 2 + 4], fill=BLACK)
            d.rectangle([cx0 + w_ - 3, y0 + h_ // 2 - 4, cx0 + w_, y0 + h_ // 2 + 4], fill=BLACK)
        px_draw(px, x0 + w_ + gap + w_ - 6 * len(suffix) - 8, y0 + h_ - 14, suffix, 1, DIMU)
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
    label_centered(px, 160, "Monday 10 Aug", 2, DIMU, face == "sans")
    px_centered(px, 214, "America/Denver", 1, DGRAY)
    save(img, f"clock_{face}")

# ---- weather icons, gfxWxIcon transcribed exactly (WeatherMode.cpp) -----------
C_SUN = c565(0xFEA0); C_MOONC = c565(0xE71C); C_CLOUDM = c565(0xAD75)
C_CLOUDD = c565(0x632C); C_RAINB = c565(0x34BF); C_BOLT = c565(0xFFE0)
C_FOGL = c565(0x8410)

def wx_sun(d, cx, cy, r, col):
    rr = r * 5 // 8
    d.ellipse([cx - (rr - 3), cy - (rr - 3), cx + (rr - 3), cy + (rr - 3)], fill=col)
    dirs = [(0, -1), (1, -1), (1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1)]
    for dx, dy in dirs:
        diag = dx and dy
        a, b = rr, (r * 3 // 4 if diag else r)
        d.line([cx + dx * a, cy + dy * a, cx + dx * b, cy + dy * b], fill=col)
        d.line([cx + dx * a + (1 if dy else 0), cy + dy * a + (1 if dx else 0),
                cx + dx * b + (1 if dy else 0), cy + dy * b + (1 if dx else 0)], fill=col)

def wx_moon(d, cx, cy, r):
    rr = r * 5 // 8
    d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=C_MOONC)
    r2 = r // 2
    d.ellipse([cx + r // 3 - r2, cy - r // 4 - r2, cx + r // 3 + r2, cy - r // 4 + r2], fill=BLACK)

def wx_cloud(d, cx, cy, r, col):
    by = cy + r // 4
    r1 = r // 2 + 1
    d.ellipse([cx - r // 2 - r1, by - r // 4 - r1, cx - r // 2 + r1, by - r // 4 + r1], fill=col)
    r2 = r * 5 // 8
    d.ellipse([cx + r // 8 - r2, by - r // 2 - r2, cx + r // 8 + r2, by - r // 2 + r2], fill=col)
    d.rounded_rectangle([cx - r + 2, by - r // 4, cx + r - 2, by + r // 4], r // 4, fill=col)

def wx_icon(d, klass, day, cx, cy, r):
    if klass == 0:                       # clear
        wx_sun(d, cx, cy, r, C_SUN) if day else wx_moon(d, cx, cy, r)
    elif klass == 1:                     # partly
        wx_sun(d, cx - r // 3, cy - r // 3, r * 7 // 12, C_SUN)
        wx_cloud(d, cx + r // 8, cy + r // 6, r * 3 // 4, C_CLOUDM)
    elif klass == 2:                     # overcast
        wx_cloud(d, cx, cy - r // 6, r, C_CLOUDD)
        wx_cloud(d, cx - r // 8, cy, r, C_CLOUDM)
    elif klass == 3:                     # fog
        wx_cloud(d, cx, cy - r // 3, r * 7 // 8, C_CLOUDM)
        for i in range(3):
            y = cy + r // 4 + i * (r // 4 + 1)
            d.rectangle([cx - r + 2 + (i % 2) * (r // 4), y,
                         cx - r + 2 + (i % 2) * (r // 4) + r * 3 // 2, y + 1], fill=C_FOGL)
    elif klass == 4:                     # rain
        wx_cloud(d, cx, cy - r // 3, r * 7 // 8, C_CLOUDM)
        for i in range(3):
            x = cx - r // 2 + i * (r // 2)
            y = cy + r // 4
            d.line([x, y, x - r // 6, y + r // 3], fill=C_RAINB)
            d.line([x + 1, y, x + 1 - r // 6, y + r // 3], fill=C_RAINB)
    elif klass == 5:                     # snow
        wx_cloud(d, cx, cy - r // 3, r * 7 // 8, C_CLOUDM)
        fr = 2 if r > 20 else 1
        for i in range(3):
            x = cx - r // 2 + i * (r // 2)
            y = cy + r // 3 + (i % 2) * (r // 6)
            d.ellipse([x - fr, y - fr, x + fr, y + fr], fill=WHITE)
    elif klass == 6:                     # storm
        wx_cloud(d, cx, cy - r // 3, r * 7 // 8, C_CLOUDD)
        x0, y0, w, h = cx - r // 2, cy - r // 8, r, r * 7 // 8
        P = lambda px_, py_: (x0 + px_ * w // 100, y0 + py_ * h // 100)
        d.polygon([P(60, 0), P(20, 55), P(45, 55)], fill=C_BOLT)
        d.polygon([P(60, 0), P(45, 55), P(75, 0)], fill=C_BOLT)
        d.polygon([P(52, 40), P(80, 40), P(30, 100)], fill=C_BOLT)

def weather(face):
    img, px, d = screen()
    sans = face == "sans"
    # current: partly cloudy, 73 F, H/L
    wx_icon(d, 1, True, 52, 62, 34)
    num = "73"
    if sans:
        f = num_face_for(num, 100)
        w = sans_w(NUM[f], num)
        sans_draw(px, NUM[f], 104, 28 + NUM_META[f], num, WHITE)
        x = 104 + w
    else:
        px_draw(px, 104, 28, num, 5, WHITE)
        x = 104 + px_w(num, 5)
    d.ellipse([x + 3, 30, x + 11, 38], outline=WHITE, width=2)
    label(px, x + 16, 30, "F", 2, DIMU, sans)
    label(px, 104, 88, "H 96", 2, WHITE, sans)
    label(px, 104 + label_w("H 96", 2, sans) + 14, 88, "L 68", 2, DIMU, sans)
    d.rectangle([12, 118, 227, 118], fill=c565(0x4208))
    days = [("TUE", 4, "95", "66"), ("WED", 6, "94", "64"), ("THU", 0, "86", "62")]
    for i, (lbl, k, hi, lo) in enumerate(days):
        cx = 40 + i * 80
        label(px, cx - label_w(lbl, 1, False) // 2, 128, lbl, 1, DIMU, False)
        wx_icon(d, k, True, cx, 162, 17)
        label(px, cx - label_w(hi, 2, sans) // 2, 186, hi, 2, WHITE, sans)
        label(px, cx - label_w(lo, 2, sans) // 2, 206, lo, 2, DIMU, sans)
    px_centered(px, 228, "just updated", 1, c565(0x4208))
    save(img, f"weather_{face}")

def weather_icons():
    """All seven icon classes plus the moon, labelled, for review."""
    img, px, d = screen()
    names = ["CLEAR", "PARTLY", "CLOUD", "FOG", "RAIN", "SNOW", "STORM", "NIGHT"]
    for i, nm in enumerate(names):
        cx, cy = 40 + (i % 3) * 80, 44 + (i // 3) * 76
        if nm == "NIGHT":
            wx_icon(d, 0, False, cx, cy, 24)
        else:
            wx_icon(d, i, True, cx, cy, 24)
        px_draw(px, cx - px_w(nm, 1) // 2, cy + 34, nm, 1, DIMU)
    save(img, "weather_icons")

def sheet(name, variants):
    """Side-by-side comparison sheet with a caption strip, for review at a glance."""
    caps = {"pixel": "PIXEL", "sans": "SANS", "7seg": "SEVEN-SEGMENT",
            "analog": "ANALOG", "flip": "FLIP"}
    tiles = [Image.open(OUT / f"{name}_{v}.png") for v in variants]
    tw, th = tiles[0].size
    pad, strip = 12, 48
    img = Image.new("RGB", (len(tiles) * (tw + pad) + pad, th + strip + pad), (13, 17, 28))
    d = ImageDraw.Draw(img)
    for i, (tile, v) in enumerate(zip(tiles, variants)):
        x = pad + i * (tw + pad)
        cap = caps[v]
        cx = x + (tw - px_w(cap, 1) * 2) // 2
        cimg = Image.new("RGB", (px_w(cap, 1), 10), (13, 17, 28))
        px_draw(cimg.load(), 0, 1, cap, 1, (176, 182, 196), W=cimg.width, H=10)
        img.paste(cimg.resize((cimg.width * 2, 20), Image.NEAREST), (cx, 14))
        img.paste(tile, (x, strip))
    img.save(OUT / f"sheet_{name}.png")
    print(f"  sheet_{name}.png")

if __name__ == "__main__":
    print("rendering previews:")
    for f in ("pixel", "sans"):
        ticker(f)
        usage(f)
        miner(f)
    for f in ("pixel", "sans", "7seg", "analog", "flip"):
        clock(f)
    weather("pixel")
    weather("sans")
    weather_icons()
    sheet("ticker", ("pixel", "sans"))
    sheet("usage", ("pixel", "sans"))
    sheet("miner", ("pixel", "sans"))
    sheet("clock", ("pixel", "sans", "7seg", "analog", "flip"))
    sheet("weather", ("pixel", "sans"))
