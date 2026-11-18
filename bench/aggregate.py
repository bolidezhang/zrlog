"""汇总多轮基准结果：对每库的延迟各百分位与吞吐取中位数和范围。"""
import statistics as st
import sys
from pathlib import Path

cols = ['avg', 'min', 'p50', 'p90', 'p99', 'p99.9', 'max']
root = Path(sys.argv[1] if len(sys.argv) > 1 else 'results')
libs = [p.stem for p in sorted(root.glob('*.txt')) if p.stem != 'env']

lat_data = {}
thr_data = {}
for lib in libs:
    lat = {c: [] for c in cols}
    thr = []
    for line in (root / f'{lib}.txt').read_text(encoding='utf-8', errors='replace').splitlines():
        p = line.split()
        if len(p) == 8 and p[0] == lib:
            for i, c in enumerate(cols):
                lat[c].append(float(p[i + 1]))
        elif len(p) == 4 and p[0] == lib:
            thr.append(float(p[1]))
    lat_data[lib] = lat
    thr_data[lib] = thr

print('=' * 78)
print('LATENCY (ns, median of runs; [min..max] across runs)')
print('=' * 78)
hdr = '%-9s' % 'lib' + ''.join('%12s' % c for c in cols)
print(hdr)
for lib in libs:
    row = '%-9s' % lib
    for c in cols:
        v = lat_data[lib][c]
        if not v:
            row += '%12s' % '-'
        else:
            row += '%12s' % ('%.1f' % st.median(v))
    print(row)

print()
print('=' * 78)
print('THROUGHPUT (M logs/s)')
print('=' * 78)
print('%-9s %10s %10s %10s %10s %6s' % ('lib', 'median', 'min', 'max', 'stdev', 'n'))
for lib in libs:
    v = thr_data[lib]
    if not v:
        continue
    sd = st.stdev(v) if len(v) > 1 else 0
    print('%-9s %10.2f %10.2f %10.2f %10.2f %6d' %
          (lib, st.median(v), min(v), max(v), sd, len(v)))

# 延迟范围表
print()
print('=' * 78)
print('LATENCY RANGE (min..max across runs) — 稳定性参考')
print('=' * 78)
for lib in libs:
    parts = []
    for c in ['p50', 'p99', 'p99.9']:
        v = lat_data[lib][c]
        if v:
            parts.append('%s %.0f..%.0f' % (c, min(v), max(v)))
    print('%-9s %s' % (lib, '  '.join(parts)))
