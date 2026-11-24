#!/usr/bin/env python3
# Makes vcd_font_cjk.h: the Japanese and Chinese characters of the menus
# (every character from U+2000 up found in vcd_player.c and mediacenter.c),
# 1 bit per pixel, from Noto Sans CJK (SIL Open Font License 1.1): its JP
# face for Japanese, its SC face for Simplified Chinese. One table per size
# the menus and the controls over the video use (the em box of the glyph,
# w x h pixels): 24x24 menus at 480/576 lines and controls at 1280 wide,
# 24x12 menus at 240/288 lines (each line twice as tall on the CRT), 18x24
# and 18x30 controls on 480 and 576 TV lines, 12x12 controls at 240/288
# lines, 36x36 controls at 1920 wide. Square 24 and 36 are drawn by FreeType
# at that size (Bold); the others are reduced from a 96 pixel drawing,
# Bold, or Regular for 24x12 (Bold fills in the strokes at 12 lines).
# Rows top down, each row 1 byte per 8 pixels, the leftmost pixel in bit 7.
# Usage: make_cjk_font.py [font folder] > vcd_font_cjk.h
import os, sys
from PIL import Image, ImageDraw, ImageFont

here = os.path.dirname(os.path.abspath(__file__))
fonts = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/opentype/noto"
BOLD = os.path.join(fonts, "NotoSansCJK-Bold.ttc")
REGULAR = os.path.join(fonts, "NotoSansCJK-Regular.ttc")
FACES = (("ja", 0), ("zh", 2))  # face index in the .ttc: 0 JP, 2 SC
# (w, h, font, threshold, direct): direct = drawn at that size by FreeType
SIZES = ((24, 24, BOLD, 128, True), (36, 36, BOLD, 128, True), (24, 12, REGULAR, 110, False),
         (18, 24, BOLD, 150, False), (18, 30, BOLD, 150, False), (12, 12, BOLD, 150, False))

chars = set()
for f in ("vcd_player.c", "../mediacenter/mediacenter.c"):
    chars |= {c for c in open(os.path.join(here, f), encoding="utf-8").read() if ord(c) >= 0x2000}
cps = sorted(ord(c) for c in chars)
assert cps and cps[-1] < 0x10000

def em(font, size, cp):
    img = Image.new("L", (size, size), 0)
    # the em box: baseline 0.88 em from the top (Noto CJK ascender)
    ImageDraw.Draw(img).text((0, round(size * 0.88)), chr(cp), font=font, fill=255, anchor="ls")
    return img

def glyph(w, h, path, idx, th, direct, cp):
    if direct:
        img = em(ImageFont.truetype(path, w, index=idx), w, cp)
    else:
        img = em(ImageFont.truetype(path, 96, index=idx), 96, cp).resize((w, h), Image.BOX)
    px = img.load()
    out = []
    for y in range(h):
        for x0 in range(0, w, 8):
            b = 0
            for i in range(8):
                if x0 + i < w and px[x0 + i, y] >= th:
                    b |= 0x80 >> i
            out.append(b)
    return out

print("// Made by make_cjk_font.py from Noto Sans CJK (SIL OFL 1.1). Do not edit.")
print("#define CJK_N %d" % len(cps))
print("static const unsigned short cjk_cp[CJK_N] = {")
for i in range(0, len(cps), 12):
    print("\t" + " ".join("0x%04X," % c for c in cps[i:i + 12]))
print("};")
for name, idx in FACES:
    for w, h, path, th, direct in SIZES:
        n = (w + 7) // 8 * h
        print("static const unsigned char cjk_%s_%dx%d[CJK_N][%d] = {" % (name, w, h, n))
        for cp in cps:
            print("\t{ " + ",".join("0x%02X" % b for b in glyph(w, h, path, idx, th, direct, cp)) + " }, // U+%04X" % cp)
        print("};")
print("typedef struct { int w, h; const unsigned char *ja, *zh; } cjk_size_t;")
print("static const cjk_size_t cjk_sizes[] = {")
for w, h, path, th, direct in SIZES:
    print("\t{ %d, %d, cjk_ja_%dx%d[0], cjk_zh_%dx%d[0] }," % (w, h, w, h, w, h))
print("};")
