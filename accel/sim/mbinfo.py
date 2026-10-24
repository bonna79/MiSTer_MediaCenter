#!/usr/bin/env python3
"""Prints the commands of one macroblock: mbinfo.py <prefix> <picture> <mbx> <mby>"""
import sys
import numpy as np
c = np.fromfile(sys.argv[1] + '.cmd', dtype='<u4')
pic, mx, my = (int(v) for v in sys.argv[2:5])
p = -1
i = 0
while i < len(c):
    w = int(c[i])
    t = w >> 24
    if t == 0xA1:
        p += 1
        i += 2
    elif t == 0xA2:
        n = (w & 63) + (((w >> 6) & 63) if (w >> 12) & 1 else 0)
        i += 1 + 3 * n
    elif t == 0xA4 or t == 0xA0:
        i += 1
    elif t == 0xA3:
        typ = (w >> 16) & 15
        if typ == 4:
            n = 97
        else:
            k = 6 if typ else 38
            n = k + 1 + int(c[i + k])
        if p == pic and (w & 255) == mx and ((w >> 8) & 255) == my:
            words = [int(v) for v in c[i:i + n]]
            print('type', typ, 'mask %06x t8 %d' % (words[1] & 0xFFFFFF, (words[1] >> 24) & 1))
            if typ:
                print('modes', [(words[2 + j // 8] >> (4 * (j % 8))) & 15 for j in range(16)])
                print('tr %04x tl %04x cmode %d i16 %d' % (words[4] & 0xFFFF, words[4] >> 16, words[5] & 15, (words[5] >> 4) & 15))
            else:
                print('refs', [hex(v) for v in words[2:6]])
                print('mv', [((v & 0xFFFF) - (v & 0x8000) * 2, (v >> 16) - (v >> 31) * 65536) for v in words[6:22]])
            k = 6 if typ else 38
            print('coef', [((v >> 22) & 31, (v >> 16) & 63, (v & 0xFFFF) - (v & 0x8000) * 2) for v in words[k + 1:]])
        i += n
    else:
        raise SystemExit('bad tag at %d' % i)
