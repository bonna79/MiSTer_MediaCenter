#!/usr/bin/env python3
# Test vectors for acc_idct: random sparse blocks and the model's result.
import random
import sys
sys.path.insert(0, __file__.rsplit('/', 2)[0])
from model import idct4, idct8

random.seed(1)
out = open(sys.argv[1], 'w')
for t in range(4000):
    t8 = t & 1
    n = 64 if t8 else 16
    c = [0] * n
    k = random.choice([1, 2, 4, 8, n])
    big = random.choice([64, 512, 4096, 32767])
    for _ in range(k):
        c[random.randrange(n)] = random.randint(-big, big - 1)
    r = idct8(c) if t8 else idct4(c)
    nz = [(i, v) for i, v in enumerate(c) if v]
    out.write('%d %d %s %s\n' % (t8, len(nz), ' '.join('%d %d' % (i, v & 0xFFFF) for i, v in nz),
                               ' '.join('%d' % (r[i][j] & 0xFFFF) for i in range(len(r)) for j in range(len(r)))))
