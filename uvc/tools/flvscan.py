"""Scan a mid-stream UniFi FLV capture: resync on tag framing (11-byte header,
body, PreviousTagSize == 11+size, 16-byte trailer). Summarize tag types, audio
headers and trailer clocks; optionally dump the first N tags."""
import sys, struct
from collections import Counter
d = open(sys.argv[1], 'rb').read()
dump = int(sys.argv[2]) if len(sys.argv) > 2 else 0
i = 0; tags = []
def plausible(i):
    if i + 15 > len(d) or d[i] not in (8, 9, 18): return 0
    size = int.from_bytes(d[i+1:i+4], 'big')
    e = i + 11 + size
    if e + 4 + 16 > len(d): return 0
    if int.from_bytes(d[e:e+4], 'big') != size + 11: return 0
    if d[e+4] != 0: return 0
    return size
synced = False
while i < len(d) - 31:
    size = plausible(i)
    if not size:
        synced = False; i += 1; continue
    if not synced:
        print(f'sync at {i}'); synced = True
    ts = int.from_bytes(d[i+4:i+7], 'big') | d[i+7] << 24
    body = d[i+11:i+11+size]
    tr = d[i+11+size+4:i+11+size+20]
    clock = int.from_bytes(tr[1:4], 'big'); ticks = int.from_bytes(tr[12:16], 'big')
    tags.append((i, d[i], size, ts, body[:6], clock, ticks, tr[4:12]))
    i += 11 + size + 20
print('tags', len(tags))
print('types', Counter(t[1] for t in tags))
print('audio head bytes', Counter(t[4][:2].hex() for t in tags if t[1] == 8).most_common(8))
print('video head bytes', Counter(t[4][:2].hex() for t in tags if t[1] == 9).most_common(8))
print('clocks by type', Counter((t[1], t[5]) for t in tags))
print('trailer middle bytes', Counter(t[7].hex() for t in tags).most_common(4))
# per-type timestamp steps
for typ in (8, 9):
    sel = [t for t in tags if t[1] == typ]
    steps = Counter(b[3] - a[3] for a, b in zip(sel, sel[1:]))
    tsteps = Counter(b[6] - a[6] for a, b in zip(sel, sel[1:]))
    sizes = Counter(t[2] for t in sel)
    print(f'type {typ}: ts steps {steps.most_common(6)}  tick steps {tsteps.most_common(6)}  sizes {sizes.most_common(5)}')
# audio by head byte
for hb, n in Counter(t[4][:1].hex() for t in tags if t[1] == 8).items():
    sel = [t for t in tags if t[1] == 8 and t[4][:1].hex() == hb]
    steps = Counter(b[3] - a[3] for a, b in zip(sel, sel[1:]))
    print(f'audio {hb}: {n} tags, ts steps {steps.most_common(4)}, clocks {Counter(t[5] for t in sel)}, sizes {Counter(t[2] for t in sel).most_common(4)}')
for t in tags[:dump]:
    print(f'@{t[0]:<9} type {t[1]:>2} size {t[2]:>6} ts {t[3]:>10} body {t[4].hex()} clock {t[5]} ticks {t[6]} ticks/ms {t[6]/t[3] if t[3] else 0:.3f}')
