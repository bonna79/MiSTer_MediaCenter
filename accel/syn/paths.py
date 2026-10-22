# Groups the worst timing paths by destination register (name without bit
# index and fitter suffixes), worst slack first: what to pipeline next.
import re
import sys

worst = {}
for line in open(sys.argv[1]):
    f = [x.strip() for x in line.split(';')]
    if len(f) < 4:
        continue
    try:
        slack = float(f[1])
    except ValueError:
        continue
    def base(n):
        n = re.sub(r'_OTERM\d+|~\w+|DUPLICATE|_RESYN\d+', '', n)
        return re.sub(r'\[\d+\]', '[]', n)
    key = base(f[3])
    if key not in worst or slack < worst[key][0]:
        worst[key] = (slack, base(f[2]))
for k, (s, frm) in sorted(worst.items(), key=lambda x: x[1][0])[:45]:
    print('%8.3f  %-70s <- %s' % (s, k[-70:], frm[-60:]))
