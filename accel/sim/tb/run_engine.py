#!/usr/bin/env python3
"""Runs tb_engine on a dump (<prefix>.cmd/.rec) and compares the pictures.
usage: run_engine.py <prefix> [pictures]"""
import os
import struct
import subprocess
import sys

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
rtl = os.path.join(here, '..', '..', 'rtl')
prefix = sys.argv[1]
npics = int(sys.argv[2]) if len(sys.argv) > 2 else 0
cmd = np.fromfile(prefix + '.cmd', dtype='<u4')
rec = open(prefix + '.rec', 'rb').read()
# expected pictures
exp = []
o = 0
while o < len(rec):
    slot, w, h = struct.unpack_from('<3I', rec, o)
    o += 12
    n = w * h * 3 // 2
    exp.append((w, h, rec[o:o + n]))
    o += n
if npics:
    exp = exp[:npics]
    # cut the commands after the last picture wanted
    ends = [i for i in range(len(cmd)) if cmd[i] >> 24 == 0xA4]
    cmd = cmd[:ends[len(exp) - 1] + 1]
w, h = exp[0][0], exp[0][1]
big = w > 512 or h > 512
work = prefix + '_tb'
os.makedirs(work, exist_ok=True)
with open(os.path.join(work, 'cmd.hex'), 'w') as f:
    f.write('\n'.join('%08x' % v for v in cmd))
defs = ['-DCMDFILE="cmd.hex"', '-DNCMD=%d' % len(cmd), '-DCMDMAX=%d' % len(cmd), '-DNPICS=%d' % len(exp)]
# memory layout: rows of 2^YS_L2 64-bit words, U after the luma rows, V after U
hh = (h + 15) // 16 * 16
if big:
    defs += ['-DSLOT_L2=19', '-DYS_L2=8', '-DCS_L2=7', '-DUOFF=278528', '-DVOFF=348160']
else:
    # the player's slots (up to 32 in use) each keep their own picture
    defs += ['-DNSLOT=32', '-DSLOT_L2=16', '-DYS_L2=6', '-DCS_L2=5', '-DUOFF=%d' % (64 * hh), '-DVOFF=%d' % (64 * hh + 32 * hh // 2)]
srcs = [os.path.join(here, 'tb_engine.sv')] + [os.path.join(rtl, f) for f in ('acc_engine.sv', 'acc_mc.sv', 'acc_idct.sv', 'acc_resid.sv', 'acc_recon.sv', 'acc_ram.sv')]
subprocess.check_call(['iverilog', '-g2012', '-o', os.path.join(work, 'sim')] + defs + srcs)
r = subprocess.run(['vvp', '-n', 'sim'], cwd=work, capture_output=True, text=True)
print(r.stdout[-3000:], r.stderr[-2000:])
out = open(os.path.join(work, 'out.yuv'), 'rb').read()
o = 0
bad = 0
for k, (w, h, e) in enumerate(exp):
    n = w * h * 3 // 2
    got = out[o:o + n]
    o += n
    if len(got) < n:
        print('picture %d: missing' % k)
        bad += 1
        break
    a = np.frombuffer(got, np.uint8)
    b = np.frombuffer(e, np.uint8)
    d = np.nonzero(a != b)[0]
    if len(d):
        bad += 1
        i = d[0]
        if i < w * h:
            where = 'Y mb %d,%d (x %d y %d)' % (i % w // 16, i // w // 16, i % w, i // w)
        else:
            j = (i - w * h) % (w * h // 4)
            where = '%s mb %d,%d' % ('U' if i < w * h * 5 // 4 else 'V', j % (w // 2) // 8, j // (w // 2) // 8)
        print('picture %d: %d different pixels, first at %s: got %d expected %d' % (k, len(d), where, a[i], b[i]))
print('engine: %d pictures, %d different from FFmpeg' % (len(exp), bad))
sys.exit(1 if bad else 0)
