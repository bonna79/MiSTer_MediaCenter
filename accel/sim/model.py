#!/usr/bin/env python3
"""Reference model of the FPGA reconstruction engine (accelerator phase 1).

Reads the commands written by accdump (<prefix>.cmd), rebuilds every picture
with the H.264 rules (standard formulas, the way FFmpeg applies them) and
checks it against the picture FFmpeg rebuilt (<prefix>.rec).

usage: model.py <prefix> [--out <prefix>]   (--out also writes the pictures)

This model is the specification the Verilog engine is checked against: same
commands in, same pictures out. The command format is in accel/doc/comandi.md.
"""
import struct
import sys

import numpy as np


def clip(v):
    return 0 if v < 0 else (255 if v > 255 else v)


def s8(v):
    v &= 0xFF
    return v - 256 if v & 0x80 else v


def s9(v):
    v &= 0x1FF
    return v - 512 if v & 0x100 else v


def s16(v):
    v &= 0xFFFF
    return v - 65536 if v & 0x8000 else v


# ---------------------------------------------------------------- transforms

def idct4(c):
    """c: 16 coefficients, raster order (row*4+col). Returns 4x4 residual."""
    d = [c[i * 4:(i + 1) * 4] for i in range(4)]
    f = []
    for i in range(4):  # rows
        r = d[i]
        e0 = r[0] + r[2]
        e1 = r[0] - r[2]
        e2 = (r[1] >> 1) - r[3]
        e3 = r[1] + (r[3] >> 1)
        f.append([e0 + e3, e1 + e2, e1 - e2, e0 - e3])
    res = [[0] * 4 for _ in range(4)]
    for j in range(4):  # columns
        g0 = f[0][j] + f[2][j]
        g1 = f[0][j] - f[2][j]
        g2 = (f[1][j] >> 1) - f[3][j]
        g3 = f[1][j] + (f[3][j] >> 1)
        h = [g0 + g3, g1 + g2, g1 - g2, g0 - g3]
        for i in range(4):
            res[i][j] = (h[i] + 32) >> 6
    return res


def idct8_1d(d):
    a0 = d[0] + d[4]
    a4 = d[0] - d[4]
    a2 = (d[2] >> 1) - d[6]
    a6 = d[2] + (d[6] >> 1)
    b0 = a0 + a6
    b2 = a4 + a2
    b4 = a4 - a2
    b6 = a0 - a6
    a1 = -d[3] + d[5] - d[7] - (d[7] >> 1)
    a3 = d[1] + d[7] - d[3] - (d[3] >> 1)
    a5 = -d[1] + d[7] + d[5] + (d[5] >> 1)
    a7 = d[3] + d[5] + d[1] + (d[1] >> 1)
    b1 = a1 + (a7 >> 2)
    b7 = a7 - (a1 >> 2)
    b3 = a3 + (a5 >> 2)
    b5 = (a3 >> 2) - a5
    return [b0 + b7, b2 + b5, b4 + b3, b6 + b1, b6 - b1, b4 - b3, b2 - b5, b0 - b7]


def idct8(c):
    rows = [idct8_1d(c[i * 8:(i + 1) * 8]) for i in range(8)]
    res = [[0] * 8 for _ in range(8)]
    for j in range(8):
        col = idct8_1d([rows[i][j] for i in range(8)])
        for i in range(8):
            res[i][j] = (col[i] + 32) >> 6
    return res


# ---------------------------------------------------------- intra prediction
# FFmpeg mode numbers: 4x4/8x8: 0 V, 1 H, 2 DC, 3 DDL, 4 DDR, 5 VR, 6 HD,
# 7 VL, 8 HU, 9 DC left only, 10 DC top only, 11 DC 128.
# 16x16 and chroma: 0 DC, 1 H, 2 V, 3 plane, 4 DC left only, 5 DC top only,
# 6 DC 128.

def pred_dir(n, mode, top, left, tl):
    """Directional/DC prediction of an n x n block (n = 4 or 8).
    top: 2n samples p[x,-1], left: n samples p[-1,y], tl: p[-1,-1]."""
    P = [[0] * n for _ in range(n)]

    def T(x):
        return tl if x < 0 else top[x]

    def L(y):
        return tl if y < 0 else left[y]

    last = 2 * n - 1
    for y in range(n):
        for x in range(n):
            if mode == 0:
                v = top[x]
            elif mode == 1:
                v = left[y]
            elif mode == 2:
                v = (sum(top[:n]) + sum(left) + n) >> (3 if n == 4 else 4)
            elif mode == 9:
                v = (sum(left) + n // 2) >> (2 if n == 4 else 3)
            elif mode == 10:
                v = (sum(top[:n]) + n // 2) >> (2 if n == 4 else 3)
            elif mode == 11:
                v = 128
            elif mode == 3:
                if x == n - 1 and y == n - 1:
                    v = (top[last - 1] + 3 * top[last] + 2) >> 2
                else:
                    v = (top[x + y] + 2 * top[x + y + 1] + top[x + y + 2] + 2) >> 2
            elif mode == 4:
                if x > y:
                    v = (T(x - y - 2) + 2 * T(x - y - 1) + T(x - y) + 2) >> 2
                elif x < y:
                    v = (L(y - x - 2) + 2 * L(y - x - 1) + L(y - x) + 2) >> 2
                else:
                    v = (T(0) + 2 * tl + L(0) + 2) >> 2
            elif mode == 5:
                z = 2 * x - y
                if z >= 0 and z % 2 == 0:
                    v = (T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 1) >> 1
                elif z >= 0:
                    v = (T(x - (y >> 1) - 2) + 2 * T(x - (y >> 1) - 1) + T(x - (y >> 1)) + 2) >> 2
                elif z == -1:
                    v = (L(0) + 2 * tl + T(0) + 2) >> 2
                else:
                    v = (L(y - 2 * x - 1) + 2 * L(y - 2 * x - 2) + L(y - 2 * x - 3) + 2) >> 2
            elif mode == 6:
                z = 2 * y - x
                if z >= 0 and z % 2 == 0:
                    v = (L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 1) >> 1
                elif z >= 0:
                    v = (L(y - (x >> 1) - 2) + 2 * L(y - (x >> 1) - 1) + L(y - (x >> 1)) + 2) >> 2
                elif z == -1:
                    v = (L(0) + 2 * tl + T(0) + 2) >> 2
                else:
                    v = (T(x - 2 * y - 1) + 2 * T(x - 2 * y - 2) + T(x - 2 * y - 3) + 2) >> 2
            elif mode == 7:
                k = x + (y >> 1)
                if y % 2 == 0:
                    v = (top[k] + top[k + 1] + 1) >> 1
                else:
                    v = (top[k] + 2 * top[k + 1] + top[k + 2] + 2) >> 2
            elif mode == 8:
                z = x + 2 * y
                k = y + (x >> 1)
                lim = 2 * n - 3
                if z < lim and z % 2 == 0:
                    v = (left[k] + left[k + 1] + 1) >> 1
                elif z < lim:
                    v = (left[k] + 2 * left[k + 1] + left[k + 2] + 2) >> 2
                elif z == lim:
                    v = (left[n - 2] + 3 * left[n - 1] + 2) >> 2
                else:
                    v = left[n - 1]
            else:
                raise ValueError('intra mode %d' % mode)
            P[y][x] = v
    return P


def filter8(top16, left8, tl, has_tl, has_tr):
    """8x8 reference sample filtering, as FFmpeg PREDICT_8x8_LOAD_*."""
    t = top16
    ft = [0] * 16
    ft[0] = ((tl if has_tl else t[0]) + 2 * t[0] + t[1] + 2) >> 2
    for x in range(1, 7):
        ft[x] = (t[x - 1] + 2 * t[x] + t[x + 1] + 2) >> 2
    ft[7] = ((t[8] if has_tr else t[7]) + 2 * t[7] + t[6] + 2) >> 2
    if has_tr:
        for x in range(8, 15):
            ft[x] = (t[x - 1] + 2 * t[x] + t[x + 1] + 2) >> 2
        ft[15] = (t[14] + 3 * t[15] + 2) >> 2
    else:
        for x in range(8, 16):
            ft[x] = t[7]
    l = left8
    fl = [0] * 8
    fl[0] = ((tl if has_tl else l[0]) + 2 * l[0] + l[1] + 2) >> 2
    for y in range(1, 7):
        fl[y] = (l[y - 1] + 2 * l[y] + l[y + 1] + 2) >> 2
    fl[7] = (l[6] + 3 * l[7] + 2) >> 2
    ftl = (l[0] + 2 * tl + t[0] + 2) >> 2
    return ft, fl, ftl


def pred_big(n, mode, top, left, tl):
    """16x16 luma (n=16) or 8x8 chroma (n=8) prediction."""
    P = [[0] * n for _ in range(n)]
    if mode == 2:
        for y in range(n):
            P[y] = list(top[:n])
    elif mode == 1:
        for y in range(n):
            P[y] = [left[y]] * n
    elif mode == 3:
        h2 = n // 2
        H = sum((i + 1) * (top[h2 + i] - (top[h2 - 2 - i] if h2 - 2 - i >= 0 else tl)) for i in range(h2))
        V = sum((i + 1) * (left[h2 + i] - (left[h2 - 2 - i] if h2 - 2 - i >= 0 else tl)) for i in range(h2))
        a = 16 * (left[n - 1] + top[n - 1])
        if n == 16:
            b = (5 * H + 32) >> 6
            c = (5 * V + 32) >> 6
        else:
            b = (34 * H + 32) >> 6
            c = (34 * V + 32) >> 6
        for y in range(n):
            for x in range(n):
                P[y][x] = clip((a + b * (x - (h2 - 1)) + c * (y - (h2 - 1)) + 16) >> 5)
    elif n == 16:
        if mode == 0:
            v = (sum(top[:16]) + sum(left) + 16) >> 5
        elif mode == 4:
            v = (sum(left) + 8) >> 4
        elif mode == 5:
            v = (sum(top[:16]) + 8) >> 4
        elif mode == 6:
            v = 128
        else:
            raise ValueError('16x16 mode %d' % mode)
        P = [[v] * 16 for _ in range(16)]
    else:
        st = [sum(top[0:4]), sum(top[4:8])]
        sl = [sum(left[0:4]), sum(left[4:8])]
        for by in range(2):
            for bx in range(2):
                if mode == 0:
                    if bx == by:
                        v = (st[bx] + sl[by] + 4) >> 3
                    elif bx == 1:  # top right: top only
                        v = (st[1] + 2) >> 2
                    else:  # bottom left: left only
                        v = (sl[1] + 2) >> 2
                elif mode == 4:
                    v = (sl[by] + 2) >> 2
                elif mode == 5:
                    v = (st[bx] + 2) >> 2
                elif mode == 6:
                    v = 128
                else:
                    raise ValueError('chroma mode %d' % mode)
                for y in range(4):
                    for x in range(4):
                        P[by * 4 + y][bx * 4 + x] = v
    return P


# ------------------------------------------------------------ inter predict

def luma_qpel(ref, W, H, x0, y0, mvx, mvy, w, h):
    """w x h luma prediction at (x0, y0) with quarter-sample vector."""
    def G(x, y):
        return int(ref[min(max(y, 0), H - 1), min(max(x, 0), W - 1)])

    def tap(a, b, c, d, e, f):
        return a - 5 * b + 20 * c + 20 * d - 5 * e + f

    xi = x0 + (mvx >> 2)
    yi = y0 + (mvy >> 2)
    xf = mvx & 3
    yf = mvy & 3
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            X = xi + x
            Y = yi + y

            def b1(xx, yy):  # half sample right of (xx, yy), unscaled
                return tap(G(xx - 2, yy), G(xx - 1, yy), G(xx, yy), G(xx + 1, yy), G(xx + 2, yy), G(xx + 3, yy))

            def h1(xx, yy):  # half sample below
                return tap(G(xx, yy - 2), G(xx, yy - 1), G(xx, yy), G(xx, yy + 1), G(xx, yy + 2), G(xx, yy + 3))

            def bb(xx, yy):
                return clip((b1(xx, yy) + 16) >> 5)

            def hh(xx, yy):
                return clip((h1(xx, yy) + 16) >> 5)

            def jj(xx, yy):
                j1 = tap(b1(xx, yy - 2), b1(xx, yy - 1), b1(xx, yy), b1(xx, yy + 1), b1(xx, yy + 2), b1(xx, yy + 3))
                return clip((j1 + 512) >> 10)

            g = G(X, Y)
            if (xf, yf) == (0, 0):
                v = g
            elif (xf, yf) == (0, 1):
                v = (g + hh(X, Y) + 1) >> 1
            elif (xf, yf) == (0, 2):
                v = hh(X, Y)
            elif (xf, yf) == (0, 3):
                v = (G(X, Y + 1) + hh(X, Y) + 1) >> 1
            elif (xf, yf) == (1, 0):
                v = (g + bb(X, Y) + 1) >> 1
            elif (xf, yf) == (2, 0):
                v = bb(X, Y)
            elif (xf, yf) == (3, 0):
                v = (G(X + 1, Y) + bb(X, Y) + 1) >> 1
            elif (xf, yf) == (2, 2):
                v = jj(X, Y)
            elif (xf, yf) == (1, 1):
                v = (bb(X, Y) + hh(X, Y) + 1) >> 1
            elif (xf, yf) == (3, 1):
                v = (bb(X, Y) + hh(X + 1, Y) + 1) >> 1
            elif (xf, yf) == (1, 3):
                v = (hh(X, Y) + bb(X, Y + 1) + 1) >> 1
            elif (xf, yf) == (3, 3):
                v = (hh(X + 1, Y) + bb(X, Y + 1) + 1) >> 1
            elif (xf, yf) == (2, 1):
                v = (bb(X, Y) + jj(X, Y) + 1) >> 1
            elif (xf, yf) == (2, 3):
                v = (jj(X, Y) + bb(X, Y + 1) + 1) >> 1
            elif (xf, yf) == (1, 2):
                v = (hh(X, Y) + jj(X, Y) + 1) >> 1
            else:  # (3, 2)
                v = (jj(X, Y) + hh(X + 1, Y) + 1) >> 1
            out[y][x] = v
    return out


def chroma_pel(ref, W, H, x0, y0, mvx, mvy, w, h):
    def C(x, y):
        return int(ref[min(max(y, 0), H - 1), min(max(x, 0), W - 1)])
    xi = x0 + (mvx >> 3)
    yi = y0 + (mvy >> 3)
    xf = mvx & 7
    yf = mvy & 7
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            X = xi + x
            Y = yi + y
            out[y][x] = ((8 - xf) * (8 - yf) * C(X, Y) + xf * (8 - yf) * C(X + 1, Y) +
                         (8 - xf) * yf * C(X, Y + 1) + xf * yf * C(X + 1, Y + 1) + 32) >> 6
    return out


def biweight(A, B, w0, w1, o, d):
    # FFmpeg biweight: ((a w0 + b w1 + ((o + 1) | 1) << d) >> (d + 1)), clipped
    off = ((o + 1) | 1) << d
    return [[clip((a * w0 + b * w1 + off) >> (d + 1)) for a, b in zip(ra, rb)] for ra, rb in zip(A, B)]


def weight(P, w, o, d):
    for row in P:
        for i, v in enumerate(row):
            if d >= 1:
                row[i] = clip(((v * w + (1 << (d - 1))) >> d) + o)
            else:
                row[i] = clip(v * w + o)


# -------------------------------------------------------------------- engine

def blk4_pos(b):
    return ((b >> 2) & 1) * 8 + (b & 1) * 4, (b >> 3) * 8 + ((b >> 1) & 1) * 4


class Engine:
    def __init__(self):
        self.frames = {}
        self.cur = None
        self.refs = [[], []]
        self.stats = {}

    def count(self, k):
        self.stats[k] = self.stats.get(k, 0) + 1

    def pic(self, slot, wmb, hmb):
        self.W = wmb * 16
        self.H = hmb * 16
        self.cur = [np.zeros((self.H, self.W), np.uint8),
                    np.zeros((self.H // 2, self.W // 2), np.uint8),
                    np.zeros((self.H // 2, self.W // 2), np.uint8)]
        self.frames[slot] = self.cur
        self.slot = slot

    # neighbour samples of the current picture (only what H.264 allows)
    def top(self, p, x, y, n):
        img = self.cur[p]
        return [int(img[y - 1, min(x + i, img.shape[1] - 1)]) if y > 0 else 0 for i in range(n)]

    def left(self, p, x, y, n):
        img = self.cur[p]
        return [int(img[y + i, x - 1]) if x > 0 else 0 for i in range(n)]

    def tl(self, p, x, y):
        return int(self.cur[p][y - 1, x - 1]) if x > 0 and y > 0 else 0

    def put(self, p, x, y, P):
        img = self.cur[p]
        for j, row in enumerate(P):
            img[y + j, x:x + len(row)] = row

    def add_res(self, p, x, y, R):
        img = self.cur[p]
        for j, row in enumerate(R):
            for i, r in enumerate(row):
                img[y + j, x + i] = clip(int(img[y + j, x + i]) + r)

    def mb(self, words):
        w0 = words[0]
        mbx = w0 & 0xFF
        mby = (w0 >> 8) & 0xFF
        typ = (w0 >> 16) & 0xF
        X = mbx * 16
        Y = mby * 16
        if typ == 4:  # PCM
            self.count('pcm')
            data = b''.join(struct.pack('<I', v) for v in words[1:97])
            for j in range(16):
                self.cur[0][Y + j, X:X + 16] = list(data[j * 16:j * 16 + 16])
            for p in (1, 2):
                base = 256 + (p - 1) * 64
                for j in range(8):
                    self.cur[p][Y // 2 + j, X // 2:X // 2 + 8] = list(data[base + j * 8:base + j * 8 + 8])
            return 97
        mask = words[1] & 0xFFFFFF
        t8 = (words[1] >> 24) & 1
        if typ != 0:
            m = [(words[2] >> (4 * i)) & 15 for i in range(8)] + [(words[3] >> (4 * i)) & 15 for i in range(8)]
            tr = words[4] & 0xFFFF
            tlm = words[4] >> 16
            cmode = words[5] & 15
            i16 = (words[5] >> 4) & 15
            k = 6
        else:
            refw = words[2:6]
            mvs = [words[6 + i] for i in range(32)]   # list 0, then list 1
            k = 38
        n = words[k]
        coef = {}
        for c in words[k + 1:k + 1 + n]:
            b = (c >> 22) & 31
            coef.setdefault(b, [0] * (64 if (b < 16 and t8) else 16))[(c >> 16) & 63] = s16(c)
        used = k + 1 + n

        def res4(b):
            return idct4(coef[b]) if b in coef else None

        if typ == 1:
            self.count('i4x4')
            for b in range(16):
                bx, by = blk4_pos(b)
                x, y = X + bx, Y + by
                top = self.top(0, x, y, 8)
                if not ((tr << b) & 0x8000):
                    top[4:8] = [top[3]] * 4
                P = pred_dir(4, m[b], top, self.left(0, x, y, 4), self.tl(0, x, y))
                self.put(0, x, y, P)
                r = res4(b)
                if r:
                    self.add_res(0, x, y, r)
        elif typ == 2:
            self.count('i8x8')
            for b in (0, 4, 8, 12):
                bx, by = (b >> 2 & 1) * 8, (b >> 3) * 8
                x, y = X + bx, Y + by
                has_tl = (tlm << b) & 0x8000
                has_tr = (tr << b) & 0x4000
                ft, fl, ftl = filter8(self.top(0, x, y, 16), self.left(0, x, y, 8), self.tl(0, x, y), has_tl, has_tr)
                P = pred_dir(8, m[b], ft, fl, ftl)
                self.put(0, x, y, P)
                if b in coef:
                    self.add_res(0, x, y, idct8(coef[b]))
        elif typ == 3:
            self.count('i16x16')
            P = pred_big(16, i16, self.top(0, X, Y, 16), self.left(0, X, Y, 16), self.tl(0, X, Y))
            self.put(0, X, Y, P)
            for b in range(16):
                r = res4(b)
                if r:
                    bx, by = blk4_pos(b)
                    self.add_res(0, X + bx, Y + by, r)
        else:
            self.count('inter')
            for b in range(16):
                bx, by = blk4_pos(b)
                q = (by >> 3) * 2 + (bx >> 3)
                rr = (refw[q] & 0x7F, (refw[q] >> 7) & 0x7F)
                iw0 = s9(refw[q] >> 16)            # implicit weight of list 0
                preds = []
                for l in (0, 1):
                    if rr[l] & 0x40:
                        continue
                    rs, lw, lo, cw = self.refs[l][rr[l]]
                    ref = self.frames[rs]
                    mv = mvs[l * 16 + b]
                    mvx = s16(mv)
                    mvy = s16(mv >> 16)
                    P = luma_qpel(ref[0], self.W, self.H, X + bx, Y + by, mvx, mvy, 4, 4)
                    C = [chroma_pel(ref[p], self.W // 2, self.H // 2, (X + bx) // 2, (Y + by) // 2, mvx, mvy, 2, 2)
                         for p in (1, 2)]
                    preds.append((P, C, lw, lo, cw))
                if not preds:
                    raise ValueError('inter block without a list')
                if len(preds) == 1:
                    P, C, lw, lo, cw = preds[0]
                    if self.use_w == 1:
                        weight(P, lw, lo, self.ld)
                    if self.use_w == 1 and self.use_wc:
                        for p in (1, 2):
                            weight(C[p - 1], cw[p - 1][0], cw[p - 1][1], self.cd)
                else:
                    self.count('bi')
                    (P0, C0, lw0, lo0, cw0), (P1, C1, lw1, lo1, cw1) = preds
                    if self.use_w == 2:
                        pl = (iw0, 64 - iw0, 0, 5)
                        pc = [pl, pl]
                    elif self.use_w == 1:
                        pl = (lw0, lw1, lo0 + lo1, self.ld)
                        pc = [(cw0[k][0], cw1[k][0], cw0[k][1] + cw1[k][1], self.cd) for k in (0, 1)]
                    else:
                        pl = (1, 1, 0, 0)
                        pc = [pl, pl]
                    P = biweight(P0, P1, *pl)
                    C = [biweight(C0[k], C1[k], *pc[k]) for k in (0, 1)]
                self.put(0, X + bx, Y + by, P)
                for p in (1, 2):
                    self.put(p, (X + bx) // 2, (Y + by) // 2, C[p - 1])
            if t8:
                for b in (0, 4, 8, 12):
                    if b in coef:
                        self.add_res(0, X + (b >> 2 & 1) * 8, Y + (b >> 3) * 8, idct8(coef[b]))
            else:
                for b in range(16):
                    r = res4(b)
                    if r:
                        bx, by = blk4_pos(b)
                        self.add_res(0, X + bx, Y + by, r)
        if typ != 0:
            for p in (1, 2):
                x, y = X // 2, Y // 2
                P = pred_big(8, cmode, self.top(p, x, y, 8), self.left(p, x, y, 8), self.tl(p, x, y))
                self.put(p, x, y, P)
        for p in (1, 2):
            for kk in range(4):
                b = 16 + (p - 1) * 4 + kk
                r = res4(b)
                if r:
                    self.add_res(p, X // 2 + (kk & 1) * 4, Y // 2 + (kk >> 1) * 4, r)
        return used


def run(prefix, out=None):
    cmd = np.fromfile(prefix + '.cmd', dtype='<u4')
    rec = open(prefix + '.rec', 'rb')
    eng = Engine()
    i = 0
    npic = 0
    bad = 0
    fo = open(out + '.yuv', 'wb') if out else None
    while i < len(cmd):
        w = int(cmd[i])
        tag = w >> 24
        if tag == 0xA1:
            eng.pic(w & 0xFF, int(cmd[i + 1]) & 0xFF, (int(cmd[i + 1]) >> 8) & 0xFF)
            i += 2
        elif tag == 0xA2:
            n0 = w & 63
            n1 = (w >> 6) & 63
            two = (w >> 12) & 1
            eng.use_w = (w >> 13) & 3
            eng.use_wc = (w >> 15) & 1
            eng.ld = (w >> 16) & 15
            eng.cd = (w >> 20) & 15
            i += 1
            eng.refs = [[], []]
            for l, cnt in enumerate((n0, n1 if two else 0)):
                for _ in range(cnt):
                    a, b, c = (int(v) for v in cmd[i:i + 3])
                    i += 3
                    eng.refs[l].append((a & 0xFF, s9(a >> 8), s8(a >> 17),
                                        ((s9(b), s8(b >> 9)), (s9(c), s8(c >> 9)))))
        elif tag == 0xA3:
            i += eng.mb([int(v) for v in cmd[i:i + 600]])
        elif tag == 0xA0:     # padding
            i += 1
        elif tag == 0xA4:
            i += 1
            hdr = struct.unpack('<3I', rec.read(12))
            Wd, Hd = hdr[1], hdr[2]
            exp = [np.frombuffer(rec.read(Wd * Hd), np.uint8).reshape(Hd, Wd),
                   np.frombuffer(rec.read(Wd * Hd // 4), np.uint8).reshape(Hd // 2, Wd // 2),
                   np.frombuffer(rec.read(Wd * Hd // 4), np.uint8).reshape(Hd // 2, Wd // 2)]
            diffs = [int(np.count_nonzero(eng.cur[p] != exp[p])) for p in range(3)]
            if any(diffs):
                bad += 1
                ys, xs = np.nonzero(eng.cur[0] != exp[0]) if diffs[0] else np.nonzero(eng.cur[1] != exp[1])
                print('picture %d (slot %d): DIFFERENT pixels Y %d U %d V %d, first at mb %d,%d%s' % (
                    npic, eng.slot, diffs[0], diffs[1], diffs[2],
                    xs[0] // (16 if diffs[0] else 8), ys[0] // (16 if diffs[0] else 8), '' if diffs[0] else ' (chroma)'))
            if fo:
                for p in range(3):
                    fo.write(eng.cur[p].tobytes())
            npic += 1
        else:
            raise ValueError('word %d: unknown tag %08x' % (i, w))
    print('model: %d pictures, %d different from FFmpeg; macroblocks %s' % (npic, bad, eng.stats))
    return bad


if __name__ == '__main__':
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None
    sys.exit(1 if run(sys.argv[1], out) else 0)
