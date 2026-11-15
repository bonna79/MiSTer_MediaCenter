#!/usr/bin/env python3
# Makes vcd_font_latin1.h: the accented letters of Latin-1 (0xA0-0xFF) for
# the 8x8 menu font, in the style of vcd_font.h (MiSTer OSD font): the base
# letter of vcd_font.h with the accent drawn above it (capitals are drawn two
# rows shorter to leave room), so Spanish, French and German text can be
# shown. One byte per column, bit 0 is the top row, as in vcd_font.h.
# Usage: make_latin1_font.py > vcd_font_latin1.h
import os, re, sys

here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, "vcd_font.h")).read()
rows = re.findall(r"\{ *((?:0x[0-9a-fA-F]{2},?){8}) *\}", src)
assert len(rows) == 128
font = [[int(x, 16) for x in r.split(",")] for r in rows]

def bitmap(ch):  # 8 rows of 8 booleans
    cols = font[ord(ch)]
    return [[(cols[c] >> r) & 1 for c in range(8)] for r in range(8)]

def parse(lines):
    return [[1 if p == "#" else 0 for p in l.ljust(8, ".")] for l in lines]

# accents over a lowercase letter: rows 0 and 1 (the letter starts at row 2)
ACC2 = {
    "acute":  ["...##...", "..##...."],
    "grave":  ["..##....", "...##..."],
    "circ":   ["..##....", ".#..#..."],
    "uml":    ["##..##..", "........"],
    "tilde":  [".##.#...", "#.##...."],
}
# accents over a capital: row 0 only, then a blank row (the letter is drawn
# two rows shorter, rows 2-6)
ACC1 = {
    "acute": ["...##..."],
    "grave": [".##....."],
    "circ":  ["..##...."],
    "uml":   ["##..##.."],
    "tilde": [".####..."],
}

def lower(base, acc):
    b = bitmap(base)
    if base == "i":            # the accent takes the place of the dot
        b[0] = [0] * 8
    a = parse(ACC2[acc])
    return a + b[2:]

def upper(base, acc):
    b = bitmap(base)
    body = [b[r] for r in (0, 2, 3, 5, 6)]  # two rows shorter
    return parse(ACC1[acc]) + [[0] * 8] + body + [[0] * 8]

def cedilla(base):
    b = bitmap(base)
    return b[:7] + [parse(["..##...."])[0]]

def upper_cedilla():
    b = bitmap("C")
    return b[0:6] + parse(["..##....", ".##....."])

def flip(ch):                  # ¡ and ¿: upside down
    b = bitmap(ch)[:7]
    return b[::-1] + [[0] * 8]

SPECIAL = {
    0xDF: parse([".####...", "##..##..", "##.##...", "##..##..", "##..##..", "##.##...", "##......", "........"]),  # ß
    0xAB: parse(["........", "..#..#..", ".#..#...", "#..#....", ".#..#...", "..#..#..", "........", "........"]),  # «
    0xBB: parse(["........", "#..#....", ".#..#...", "..#..#..", ".#..#...", "#..#....", "........", "........"]),  # »
    0xB0: parse([".###....", "#...#...", "#...#...", ".###....", "........", "........", "........", "........"]),  # °
    0xB7: parse(["........", "........", "........", "..##....", "..##....", "........", "........", "........"]),  # ·
}

table = {}
LOW = {"a": [(0xE0, "grave"), (0xE1, "acute"), (0xE2, "circ"), (0xE3, "tilde"), (0xE4, "uml")],
       "e": [(0xE8, "grave"), (0xE9, "acute"), (0xEA, "circ"), (0xEB, "uml")],
       "i": [(0xEC, "grave"), (0xED, "acute"), (0xEE, "circ"), (0xEF, "uml")],
       "n": [(0xF1, "tilde")],
       "o": [(0xF2, "grave"), (0xF3, "acute"), (0xF4, "circ"), (0xF5, "tilde"), (0xF6, "uml")],
       "u": [(0xF9, "grave"), (0xFA, "acute"), (0xFB, "circ"), (0xFC, "uml")]}
UP = {"A": [(0xC0, "grave"), (0xC1, "acute"), (0xC2, "circ"), (0xC3, "tilde"), (0xC4, "uml")],
      "E": [(0xC8, "grave"), (0xC9, "acute"), (0xCA, "circ"), (0xCB, "uml")],
      "I": [(0xCC, "grave"), (0xCD, "acute"), (0xCE, "circ"), (0xCF, "uml")],
      "N": [(0xD1, "tilde")],
      "O": [(0xD2, "grave"), (0xD3, "acute"), (0xD4, "circ"), (0xD5, "tilde"), (0xD6, "uml")],
      "U": [(0xD9, "grave"), (0xDA, "acute"), (0xDB, "circ"), (0xDC, "uml")]}
for base, l in LOW.items():
    for code, acc in l: table[code] = lower(base, acc)
for base, l in UP.items():
    for code, acc in l: table[code] = upper(base, acc)
table[0xE7] = cedilla("c")
table[0xC7] = upper_cedilla()
table[0xA1] = flip("!")
table[0xBF] = flip("?")
table.update(SPECIAL)

def cols(bm):
    return [sum(bm[r][c] << r for r in range(8)) for c in range(8)]

out = sys.stdout
out.write("// Made by make_latin1_font.py from vcd_font.h. Do not edit.\n")
out.write("// Latin-1 0xA0-0xFF for the 8x8 menu font (all zero: no glyph).\n\n")
out.write("static const unsigned char vcd_font_latin1[96][8] =\n{\n")
for code in range(0xA0, 0x100):
    c = cols(table[code]) if code in table else [0] * 8
    out.write("\t{ " + ",".join("0x%02x" % v for v in c) + " }, // 0x%02X\n" % code)
out.write("};\n")
if len(sys.argv) > 1 and sys.argv[1] == "--show":
    for code in sorted(table):
        sys.stderr.write("%02X %s\n" % (code, bytes([code]).decode("latin-1")))
        for r in table[code]: sys.stderr.write("".join("#" if p else "." for p in r) + "\n")
