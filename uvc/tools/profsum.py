"""Summarize ncnn's per-layer timing output (NCNN_BENCHMARK build of bench):
   profsum.py prof.txt model.param"""
import re, sys, collections
lines = open(sys.argv[1]).read().split('\n')
first = None
for l in lines:
    m = re.match(r'(\S+)\s+(\S+)\s+([\d.]+)ms', l)
    if m: first = m.group(2); break
idx = [i for i, l in enumerate(lines) if re.match(r'\S+\s+%s\s' % re.escape(first), l)]
P = {}
for l in open(sys.argv[2]).read().split('\n')[2:]:
    p = l.split()
    if p and p[0].startswith('Conv'):
        kv = dict(x.split('=', 1) for x in p if '=' in x); P[p[1]] = int(kv['6'])
by = collections.Counter(); agg = collections.defaultdict(lambda: [0, 0]); rows = []
# The fastest of the runs for each layer: the times are wall clock and uvcd runs too
best = {}
for l in lines[idx[1] if len(idx) > 1 else 0:]:
    m = re.match(r'(\S+)\s+(\S+)\s+([\d.]+)ms', l)
    if m: best[m.group(2)] = min(best.get(m.group(2), 1e9), float(m.group(3)))
for l in lines[idx[-1]:]:
    m = re.match(r'(\S+)\s+(\S+)\s+([\d.]+)ms\s*\|(.*)', l)
    if not m: continue
    t = best[m.group(2)]; by[m.group(1)] += t
    sh = re.findall(r'\[\s*(\d+),\s*(\d+),\s*(\d+)', m.group(4)); k = re.search(r'kernel: (\d)', m.group(4))
    if m.group(2) in P and len(sh) == 2:
        ow, oh, oc = map(int, sh[1]); macs = P[m.group(2)] * ow * oh
        key = (m.group(1)[:14], k.group(1), 'big' if ow >= 80 else 'mid' if ow >= 40 else 'small')
        agg[key][0] += t; agg[key][1] += macs
        rows.append((t, m.group(1)[:14], m.group(2), sh, k.group(1), '%.1f ns/MAC' % (t * 1e6 / macs)))
print('total %.0f ms' % sum(by.values()))
for k, v in by.most_common(8): print('%-22s %7.1f' % (k, v))
for k, (t, m) in sorted(agg.items()): print(k, '%.0f ms' % t, '%.1fM' % (m / 1e6), '%.1f ns/MAC' % (t * 1e6 / m))
for r in sorted(rows, reverse=True)[:int(sys.argv[3]) if len(sys.argv) > 3 else 8]: print(r)
