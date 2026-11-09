#!/usr/bin/env python3
"""From the stream of "refcmd file -" writes two command files for tb_core
(accel/sim/core): <out>_36.hex, the commands the ARM writes today (3.6), and
<out>_a6.hex, the same with every CABAC slice's macroblock commands replaced
by command A6 + the slice's words for acc_parse (descriptor + slice data, as
accel/parse/sim/tb_parse.cpp builds them). Both must rebuild the same pictures.
usage: refcmd file - | mka6.py <out> [pictures]"""
import struct, sys

out = sys.argv[1]
maxp = int(sys.argv[2]) if len(sys.argv) > 2 else 0
f = sys.stdin.buffer

def rd(n):
    b = f.read(4 * n)
    if len(b) < 4 * n:
        return None
    return list(struct.unpack('<%di' % n, b))

def desc(h, slots, dsf, mapc, impl, sm4, sm8, nb, data):
    pic, slot, mbw, mbh, x0, y0, sn, typ, qp, idc, sp, rc0, rc1, lc, t8, cip, dbk, alpha, beta, cq0, cq1, uw, d8, x264, l1l = h
    m = 0xFFFFFFFF
    w = [(0x5C000000 | (slot & 255) << 16 | (slots[1][0] & 255) << 8 | (pic & 255)) & m,
         (mbw | mbh << 8 | x0 << 16 | y0 << 24) & m,
         ((sn & 0xFFFF) | qp << 16 | typ << 22 | idc << 24 | lc << 26 | dbk << 28 | uw << 30) & m,
         (rc0 | rc1 << 6 | sp << 12 | t8 << 13 | cip << 14 | d8 << 15 | x264 << 16 | l1l << 17 | (cq0 & 31) << 18 | (cq1 & 31) << 23) & m,
         ((alpha & 255) | (beta & 255) << 8) & m]
    if typ == 1 and not sp:
        w += [v & 0xFFFF for v in dsf]
        for l in range(2):
            w += [mapc[l][i] & 255 for i in range(32)]
    if uw == 2:
        for a in range(rc0):
            for b in range(rc1):
                w.append(impl[a][b] & 0x1FF)
    b = [sm4[i][j] & 255 for i in range(6) for j in range(16)]
    w += [b[4 * i] | b[4 * i + 1] << 8 | b[4 * i + 2] << 16 | b[4 * i + 3] << 24 for i in range(24)]
    if t8:
        b = [sm8[0][j] & 255 for j in range(64)] + [sm8[3][j] & 255 for j in range(64)]
        w += [b[4 * i] | b[4 * i + 1] << 8 | b[4 * i + 2] << 16 | b[4 * i + 3] << 24 for i in range(32)]
    w.append(nb)
    d = data + bytes(4)
    for i in range((nb + 3) // 4):
        w.append(d[4 * i] << 24 | d[4 * i + 1] << 16 | d[4 * i + 2] << 8 | d[4 * i + 3])
    return w

c36, ca6 = [], []
slices = []
npics = nsl = 0
while True:
    v = f.read(4)
    if len(v) < 4:
        break
    v = struct.unpack('<I', v)[0]
    if v == 0x50494331:
        n = struct.unpack('<I', f.read(4))[0]
        c = list(struct.unpack('<%dI' % n, f.read(4 * n)))
        c36 += c
        if not slices:
            ca6 += c
        else:
            i = 2
            ca6 += c[:2]
            k = 0
            while i < n:
                t = c[i] >> 24
                if t == 0xA2:
                    n0, n1, two = c[i] & 63, (c[i] >> 6) & 63, (c[i] >> 12) & 1
                    m = 1 + 3 * (n0 + (n1 if two else 0))
                    ca6 += c[i:i + m]
                    i += m
                    if k >= len(slices):
                        sys.exit('picture %d: more A2 than CABAC slices' % npics)
                    ca6.append(0xA6000000 | len(slices[k]))
                    ca6 += slices[k]
                    k += 1
                    # the slice's macroblock commands are acc_parse's now
                    while i < n and (c[i] >> 24) not in (0xA2, 0xA4):
                        tt = c[i] >> 24
                        if tt in (0xA5,):
                            i += 3
                        elif tt == 0xA0:
                            i += 1
                        elif tt == 0xA3:
                            typ = (c[i] >> 16) & 7
                            mm = 97 if typ == 4 else 2 + (4 if typ else 36)
                            if typ != 4:
                                mm += 1 + c[i + mm]
                            i += mm
                        else:
                            sys.exit('picture %d: word %08x' % (npics, c[i]))
                elif t == 0xA4:
                    ca6.append(c[i])
                    i += 1
                else:
                    sys.exit('picture %d: unexpected word %08x' % (npics, c[i]))
            if k != len(slices):
                sys.exit('picture %d: %d slices, %d A2' % (npics, len(slices), k))
        slices = []
        npics += 1
        if maxp and npics >= maxp:
            break
        continue
    if v != 0x534C0001:
        sys.exit('bad record %08x' % v)
    h = rd(25)
    slots = [rd(32), rd(32)]
    dsf = rd(32)
    mapc = [rd(48), rd(48)]
    impl = [rd(32) for _ in range(32)]
    sm4 = [rd(16) for _ in range(6)]
    sm8 = [rd(64) for _ in range(6)]
    nb = rd(1)[0]
    data = f.read((nb + 3) & ~3)
    slices.append(desc(h, slots, dsf, mapc, impl, sm4, sm8, nb, data))
    nsl += 1

for name, c in (('_36', c36), ('_a6', ca6)):
    with open(out + name + '.hex', 'w') as g:
        g.write('\n'.join('%08x' % (x & 0xFFFFFFFF) for x in c))
print('%d pictures, %d CABAC slices; words 3.6 %d, A6 %d' % (npics, nsl, len(c36), len(ca6)))
