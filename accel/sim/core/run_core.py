#!/usr/bin/env python3
"""Runs tb_core (accelerator in the menu core) on a dump (<prefix>.cmd/.rec)
and compares the pictures with FFmpeg's.
usage: run_core.py <prefix> [pictures] [ring pages] [latency]"""
import os, struct, subprocess, sys
import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, '..', '..', '..')
prefix = sys.argv[1]
npics = int(sys.argv[2]) if len(sys.argv) > 2 else 0
pages = int(sys.argv[3]) if len(sys.argv) > 3 else 6
lat = int(sys.argv[4]) if len(sys.argv) > 4 else 30
cmd = np.fromfile(prefix + '.cmd', dtype='<u4')
rec = open(prefix + '.rec', 'rb').read()
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
    ends = [i for i in range(len(cmd)) if cmd[i] >> 24 == 0xA4]
    cmd = cmd[:ends[len(exp) - 1] + 1]
work = prefix + '_core'
os.makedirs(work, exist_ok=True)
with open(os.path.join(work, 'cmd.hex'), 'w') as f:
    f.write('\n'.join('%08x' % v for v in cmd))
defs = ['-DCMDFILE="cmd.hex"', '-DNCMD=%d' % len(cmd), '-DCMDMAX=%d' % (len(cmd) + 1), '-DNPICS=%d' % len(exp),
        '-DRING_PAGES=%d' % pages, '-DLAT=%d' % lat]
rtl = os.path.join(root, 'accel', 'rtl')
srcs = [os.path.join(here, 'tb_core.sv'), os.path.join(root, 'fpga/menu/rtl/mc_acc.sv'), os.path.join(root, 'fpga/menu/rtl/mc_bus.sv')] + \
       [os.path.join(rtl, f) for f in ('acc_engine.sv', 'acc_dbk.sv', 'acc_mc.sv', 'acc_idct.sv', 'acc_resid.sv', 'acc_recon.sv', 'acc_ram.sv')]
subprocess.check_call(['iverilog', '-g2012', '-o', os.path.join(work, 'sim')] + defs + srcs)
r = subprocess.run(['vvp', '-n', 'sim'], cwd=work, capture_output=True, text=True)
print(r.stdout[-4000:], r.stderr[-2000:])
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
    d = np.nonzero(np.frombuffer(got, np.uint8) != np.frombuffer(e, np.uint8))[0]
    if len(d):
        bad += 1
        print('picture %d: %d different pixels, first at byte %d' % (k, len(d), d[0]))
print('core: %d pictures, %d different from FFmpeg' % (len(exp), bad))
sys.exit(1 if bad else 0)
