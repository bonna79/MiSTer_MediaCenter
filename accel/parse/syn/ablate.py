#!/usr/bin/env python3
# Measure only: replace the body of some FSM states of acc_parse with
# "st <= S_ERR" so Quartus drops their logic. The ALUT difference against
# the full module says what those states cost. Never used in a real build.
# ablate.py file.sv GROUP
import re, sys

GROUPS = {
    'none':    [],
    'slice':   ['S_DESC', 'S_INIT', 'S_INITW', 'S_SL0'],
    'skip':    ['S_SK0', 'S_SKE', 'S_PSK', 'S_PSK1'],
    'mbtype':  ['S_BT\\d', 'S_PT\\d', 'S_PTE', 'S_IT\\d', 'S_ITE', 'S_MTD'],
    'fill':    ['S_FC0', 'S_FCL', 'S_DISP'],
    'intra':   ['S_IN\\d', 'S_IM\\d', 'S_IMA', 'S_IWB', 'S_ICHK', 'S_CP\\d'],
    'pcm':     ['S_PCM\\d'],
    'sub8':    ['S_SB\\d', 'S_SBE', 'S_SP\\d', 'S_P8D', 'S_P8E'],
    'mvpred':  ['S_RF\\d', 'S_MV\\d', 'S_PR\\d', 'S_PRN', 'S_PV\\d', 'S_PM[A-D]'],
    'refmvd':  ['S_RD\\d', 'S_MD\\d'],
    'cbpqp':   ['S_CB\\d', 'S_T8', 'S_T8B', 'S_QD\\d'],
    'resid':   ['S_RS', 'S_RC\\d', 'S_RW'],
    'dc':      ['S_DC\\d'],
    'out':     ['S_O\\d'],
    'mbend':   ['S_E\\d'],
    'direct':  ['S_DIRE', 'S_DR0', 'S_SD[0-9A-C]', 'S_TD\\d', 'S_TP\\d', 'S_CF\\d'],
    'spdir':   ['S_SD[0-9A-C]'],
    'tpdir':   ['S_TD\\d', 'S_TP\\d'],
    'mvddec':  ['S_MD\\d'],
    'refdec':  ['S_RD\\d'],
}

# text edits instead of removed states: (old, new) pairs, each must occur
EDITS = {
    'mvcint':  [('else v = mvc[l][ix(p)];', 'else v = 32\'d0;')],
    'mvdcint': [('else v = mvdc[l][ix(p)];', 'else v = 12\'d0;')],
    'refcint': [('else r = refc[l][ix(p)];', 'else r = 6\'sd0;')],
    'cvq':     [('wire [31:0] cv_q = mvr(cr_a[6], 7\'(cr_a[5:0]));', 'wire [31:0] cv_q = 32\'d0;')],
    'cdq':     [('wire [11:0] cd_q = mdr(cr_a[6], 7\'(cr_a[5:0]));', 'wire [11:0] cd_q = 12\'d0;')],
}

src, grp = sys.argv[1], sys.argv[2]
if grp in EDITS:
    t = open(src).read()
    for a, b in EDITS[grp]:
        assert t.count(a) == 1, a
        t = t.replace(a, b)
    open(src, 'w').write(t)
    print('%s: %d edits' % (grp, len(EDITS[grp])))
    sys.exit(0)
pat = re.compile(r'^\t\t(' + '|'.join(GROUPS[grp]) + r')\s*:') if GROUPS[grp] else None
lines = open(src).read().split('\n')
# main FSM: from "case (st)" after "// ---- main FSM" to its endcase
m0 = next(i for i, l in enumerate(lines) if 'main FSM' in l)
c0 = next(i for i in range(m0, len(lines)) if lines[i].strip() == 'case (st)')
c1 = next(i for i in range(c0, len(lines)) if lines[i] == '\t\tendcase')
item = re.compile(r'^\t\t(S_[A-Z0-9_]+\s*:|default\s*:|//)')
out, i, n = lines[:c0 + 1], c0 + 1, 0
while i < c1:
    l = lines[i]
    if pat and pat.match(l):
        name = l.split(':')[0].strip()
        j = i + 1
        while j < c1 and not item.match(lines[j]):
            j += 1
        out.append('\t\t%s: st <= S_ERR;' % name)
        n += 1
        i = j
    else:
        out.append(l)
        i += 1
out += lines[c1:]
open(src, 'w').write('\n'.join(out))
print('%s: %d states removed' % (grp, n))
