# sflat.py FILE - summarize `dumpsys SurfaceFlinger --latency LAYER` output:
# cadence (present-to-present intervals in refresh periods) and queue-to-screen delay.
import sys
from collections import Counter
lines = open(sys.argv[1]).read().split('\n')
period = int(lines[0].strip()) / 1e6
rows = []
for l in lines[1:]:
    p = l.split()
    if len(p) == 3:
        a, b, c = map(int, p)
        if 0 < a < 2**62 and 0 < b < 2**62:
            rows.append((a, b, c))
pres = [r[1] for r in rows]
iv = [(y - x) / 1e6 for x, y in zip(pres, pres[1:])]
cad = Counter(round(x / period) for x in iv)
lat = sorted((b - a) / 1e6 for a, b, c in rows)
span = (pres[-1] - pres[0]) / 1e9
print('refresh %.3f ms, %d frames over %.2f s = %.2f fps' % (period, len(pres), span, (len(pres) - 1) / span))
print('cadence (refreshes per frame):', dict(sorted(cad.items())))
print('queue->screen ms: min %.2f  median %.2f  max %.2f' % (lat[0], lat[len(lat) // 2], lat[-1]))
