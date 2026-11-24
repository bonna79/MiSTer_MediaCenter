#!/usr/bin/env python3
# Makes flags.h: the flag shown beside each menu language in Impostazioni
# (Italy, United Kingdom, Spain, France, Germany, Japan, China, in the order
# of lang_codes). Drawn large, reduced to the menu sizes and every pixel set
# to the nearest colour of that flag (no blended colours: sharp on a CRT),
# with a light grey frame so dark and white flags stand out on black and on
# the blue selection bar. Sizes w x h: 30x20 for the menus at 480/576 lines,
# 30x10 at 240/288 lines (each line twice as tall on the CRT).
# Usage: make_flags.py > flags.h      (optional: make_flags.py --png dir)
import math, os, sys
from PIL import Image, ImageDraw

W, H = 360, 240          # drawing size (3:2)
SIZES = ((30, 20), (30, 10))
FRAME = (170, 170, 170)

def hexc(s): return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))

def star(d, cx, cy, r, col, rot=-90):
    pts = []
    for k in range(10):
        a = math.radians(rot + k * 36)
        rr = r if k % 2 == 0 else r * 0.382
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    d.polygon(pts, fill=col)

def flag(code):
    img = Image.new("RGB", (W, H)); d = ImageDraw.Draw(img)
    if code in ("it", "fr"):
        cols = ("009246", "FFFFFF", "CE2B37") if code == "it" else ("0055A4", "FFFFFF", "EF4135")
        for i, c in enumerate(cols): d.rectangle([i * W // 3, 0, (i + 1) * W // 3, H], fill=hexc(c))
    elif code == "de":
        for i, c in enumerate(("000000", "DD0000", "FFCE00")): d.rectangle([0, i * H // 3, W, (i + 1) * H // 3], fill=hexc(c))
    elif code == "es":
        d.rectangle([0, 0, W, H], fill=hexc("AA151B")); d.rectangle([0, H // 4, W, 3 * H // 4], fill=hexc("F1BF00"))
    elif code == "ja":
        d.rectangle([0, 0, W, H], fill=(255, 255, 255)); r = H * 3 // 10
        d.ellipse([W // 2 - r, H // 2 - r, W // 2 + r, H // 2 + r], fill=hexc("BC002D"))
    elif code == "zh":
        red, yel = hexc("DE2910"), hexc("FFDE00"); u = W / 30
        d.rectangle([0, 0, W, H], fill=red)
        star(d, 5 * u, 5 * u, 3.2 * u, yel)
        for (x, y) in ((10, 2), (12, 4), (12, 7), (10, 9)):   # small stars, a little larger to stay visible
            star(d, x * u, y * u, 1.5 * u, yel, -90 + math.degrees(math.atan2(5 - y, 5 - x)))
    elif code == "en":
        blue, white, red = hexc("012169"), (255, 255, 255), hexc("C8102E")
        d.rectangle([0, 0, W, H], fill=blue)
        # diagonals a little wider than the real ones, so they survive at 20 lines
        d.line([(0, 0), (W, H)], fill=white, width=H // 4); d.line([(0, H), (W, 0)], fill=white, width=H // 4)
        d.line([(0, 0), (W, H)], fill=red, width=H // 10); d.line([(0, H), (W, 0)], fill=red, width=H // 10)
        d.rectangle([W // 2 - H // 6, 0, W // 2 + H // 6, H], fill=white); d.rectangle([0, H // 2 - H // 6, W, H // 2 + H // 6], fill=white)
        d.rectangle([W // 2 - H // 10, 0, W // 2 + H // 10, H], fill=red); d.rectangle([0, H // 2 - H // 10, W, H // 2 + H // 10], fill=red)
    return img

CODES = ("it", "en", "es", "fr", "de", "ja", "zh")

def palette_of(img):
    return sorted({img.getpixel((x, y)) for x in range(0, W, 2) for y in range(0, H, 2)})

# China at menu size: a five pointed star drawn pixel by pixel (reduced from
# the large drawing it is a shapeless blob) and the four small stars as dots.
ZH_STAR = {20: ("..#..", "..#..", "#####", ".###.", ".#.#."), 10: ("..#..", "#####", ".#.#.")}
ZH_DOTS = {20: ((9, 2), (11, 4), (11, 6), (9, 8)), 10: ((9, 0), (11, 1), (11, 3), (9, 4))}

def small_zh(w, h):
    red, yel = hexc("DE2910"), hexc("FFDE00")
    out = Image.new("RGB", (w, h), FRAME)
    for y in range(1, h - 1):
        for x in range(1, w - 1): out.putpixel((x, y), red)
    top = 2 if h == 20 else 1
    for j, row in enumerate(ZH_STAR[h]):
        for i, c in enumerate(row):
            if c == "#": out.putpixel((1 + 2 + i, 1 + top + j), yel)
    for x, y in ZH_DOTS[h]: out.putpixel((1 + x, 1 + y), yel)
    return out

def small(code, w, h):
    if code == "zh": return small_zh(w, h)
    big = flag(code); pal = palette_of(big)
    s = big.resize((w - 2, h - 2), Image.BOX)
    out = Image.new("RGB", (w, h), FRAME)
    for y in range(h - 2):
        for x in range(w - 2):
            p = s.getpixel((x, y))
            out.putpixel((x + 1, y + 1), min(pal, key=lambda c: sum((a - b) ** 2 for a, b in zip(c, p))))
    return out

if len(sys.argv) > 2 and sys.argv[1] == "--png":
    for w, h in SIZES:
        for c in CODES: small(c, w, h).save(os.path.join(sys.argv[2], "%s_%dx%d.png" % (c, w, h)))
    sys.exit(0)

print("// Made by make_flags.py. Do not edit.")
print("#define FLAG_COUNT %d" % len(CODES))
for w, h in SIZES:
    print("static const uint32_t flag_%dx%d[FLAG_COUNT][%d] = { // 0xRRGGBB, rows top down" % (w, h, w * h))
    for c in CODES:
        im = small(c, w, h)
        print("\t{ // " + c)
        for y in range(h):
            print("\t\t" + ",".join("0x%02X%02X%02X" % im.getpixel((x, y)) for x in range(w)) + ",")
        print("\t},")
    print("};")
