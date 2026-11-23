#!/usr/bin/env python3
# Makes vcd_font_cjk.h: the Japanese and Chinese characters of the menus
# (every character from U+2000 up found in vcd_player.c and mediacenter.c),
# 1 bit per pixel, from Noto Sans CJK Bold (SIL Open Font License 1.1):
# its JP face for Japanese, its SC face for Simplified Chinese. Each glyph is
# a square em of 12, 24 and 36 pixels (24: the lists at 480 and 576 lines),
# rows top down, each row 1 byte per 8 pixels, the leftmost pixel in bit 7.
# Usage: make_cjk_font.py [NotoSansCJK-Bold.ttc] > vcd_font_cjk.h
import os, sys
from PIL import Image, ImageDraw, ImageFont

here = os.path.dirname(os.path.abspath(__file__))
ttc = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc"
FACES = (("ja", 0), ("zh", 2))  # face index in the .ttc: 0 JP, 2 SC
SIZES = (12, 24, 36)
THRESHOLD = 128

chars = set()
for f in ("vcd_player.c", "../mediacenter/mediacenter.c"):
    chars |= {c for c in open(os.path.join(here, f), encoding="utf-8").read() if ord(c) >= 0x2000}
cps = sorted(ord(c) for c in chars)
assert cps and cps[-1] < 0x10000

def glyph(font, size, cp):
    img = Image.new("L", (size, size), 0)
    # the em box: baseline 0.88 em from the top (Noto CJK ascender)
    ImageDraw.Draw(img).text((0, round(size * 0.88)), chr(cp), font=font, fill=255, anchor="ls")
    px = img.load()
    out = []
    for y in range(size):
        for x0 in range(0, size, 8):
            b = 0
            for i in range(8):
                if x0 + i < size and px[x0 + i, y] >= THRESHOLD:
                    b |= 0x80 >> i
            out.append(b)
    return out

print("// Made by make_cjk_font.py from Noto Sans CJK Bold (SIL OFL 1.1). Do not edit.")
print("#define CJK_N %d" % len(cps))
print("static const unsigned short cjk_cp[CJK_N] = {")
for i in range(0, len(cps), 12):
    print("\t" + " ".join("0x%04X," % c for c in cps[i:i + 12]))
print("};")
for name, idx in FACES:
    for size in SIZES:
        font = ImageFont.truetype(ttc, size, index=idx)
        n = (size + 7) // 8 * size
        print("static const unsigned char cjk_%s_%d[CJK_N][%d] = {" % (name, size, n))
        for cp in cps:
            print("\t{ " + ",".join("0x%02X" % b for b in glyph(font, size, cp)) + " }, // U+%04X" % cp)
        print("};")
