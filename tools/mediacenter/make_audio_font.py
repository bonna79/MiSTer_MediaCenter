#!/usr/bin/env python3
# Makes audio_font.h: the letters of the audio player screen (audio_ui.c),
# drawn once from the Inter font (SIL Open Font License) as grey levels, so
# the ARM only blends them. Three sizes: title, text, small. Codes 32-126 and
# 160-255 are Latin-1 (the accented letters); 0x80 is the ellipsis and
# 0x81-0x84 are the arrows of the joypad (from DejaVu Sans when Inter lacks
# them).
#
# usage: make_audio_font.py [out.h]
import sys
from PIL import Image, ImageDraw, ImageFont

FONTS = [
    ("title", "/usr/share/fonts/opentype/inter/InterDisplay-Bold.otf", 30),
    ("text", "/usr/share/fonts/opentype/inter/Inter-SemiBold.otf", 16),
    ("small", "/usr/share/fonts/opentype/inter/Inter-SemiBold.otf", 12),
]
FALLBACK = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
EXTRA = {0x80: "…", 0x81: "◄", 0x82: "►", 0x83: "▲", 0x84: "▼"}


def char_of(code):
    if code in EXTRA:
        return EXTRA[code]
    if 32 <= code <= 126 or 160 <= code <= 255:
        return chr(code)
    return None


def render(font, ch):
    left, top, right, bottom = font.getbbox(ch, anchor="ls")
    adv = font.getlength(ch)
    w, h = max(0, right - left), max(0, bottom - top)
    if w == 0 or h == 0:
        return round(adv), 0, 0, 0, 0, b""
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((-left, -top), ch, font=font, fill=255, anchor="ls")
    return round(adv), w, h, left, top, img.tobytes()


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "audio_font.h"
    lines = ["// Made by make_audio_font.py from the Inter font (SIL Open Font License 1.1,",
             "// https://rsms.me/inter/). Do not edit.", "",
             "typedef struct { signed char adv, w, h, x, y; unsigned int off; } afont_glyph_t;",
             "typedef struct { int size, ascent; const afont_glyph_t *g; const unsigned char *a; } afont_t;", ""]
    for name, path, size in FONTS:
        font = ImageFont.truetype(path, size)
        fb = ImageFont.truetype(FALLBACK, size)
        glyphs, data = [], bytearray()
        for code in range(256):
            ch = char_of(code)
            if ch is None:
                glyphs.append((0, 0, 0, 0, 0, 0))
                continue
            f = font
            if code >= 0x81 and code <= 0x84:
                # Inter has no joypad arrows of the right shape
                f = fb
            adv, w, h, x, y, a = render(f, ch)
            glyphs.append((adv, w, h, x, y, len(data)))
            data += a
        ascent = font.getmetrics()[0]
        lines.append("static const afont_glyph_t afont_%s_g[256] = {" % name)
        for i in range(0, 256, 4):
            lines.append("\t" + " ".join("{%d,%d,%d,%d,%d,%d}," % g for g in glyphs[i:i + 4]))
        lines.append("};")
        lines.append("static const unsigned char afont_%s_a[%d] = {" % (name, max(1, len(data))))
        for i in range(0, len(data), 24):
            lines.append("\t" + ",".join(str(b) for b in data[i:i + 24]) + ",")
        lines.append("};")
        lines.append("static const afont_t afont_%s = { %d, %d, afont_%s_g, afont_%s_a };" % (name, size, ascent, name, name))
        lines.append("")
    with open(out, "w") as f:
        f.write("\n".join(lines))


main()
