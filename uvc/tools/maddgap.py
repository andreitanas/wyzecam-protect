#!/usr/bin/env python3
"""How soon each loop of a MIPS assembly file reuses a multiply-add's result.
The T20's FPU takes 17 cycles for a madd.s but can start one every 2 or 3, so
a madd.s (or add.s/mul.s) that reads a register written fewer than 7 or so FPU
instructions earlier stalls the pipeline. Prints, for every loop body with
madd.s in it, the number of FPU operations and the shortest such distance.
   zig c++ ... -S src/fastconv.cpp -o f.s && tools/maddgap.py f.s [function-substring]"""
import re, sys
want = sys.argv[2] if len(sys.argv) > 2 else ''
func = None; blocks = {}; cur = None
for line in open(sys.argv[1]):
    m = re.match(r'^(_Z\S+|[A-Za-z_]\w*):', line)
    if m: func = m.group(1); cur = None; continue
    m = re.match(r'^(\$BB\w+):', line)
    if m: cur = (func, m.group(1)); blocks[cur] = []; continue
    if cur and re.match(r'^\s+[a-z]', line): blocks[cur].append(line.split('#')[0].split())
for (f, b), ins in blocks.items():
    if want not in f: continue
    SLOW = ('madd.s', 'mul.s', 'add.s', 'sub.s', 'msub.s')
    ops = [i for i in ins if i[0] in SLOW or i[0] in ('lwc1', 'lwxc1', 'mtc1', 'mov.s')]
    if sum(i[0] == 'madd.s' for i in ops) < 4: continue
    loop = any(i[0].startswith('b') and b in ' '.join(i) for i in ins)
    last = {}; worst = 99; seq = ops + ops if loop else ops
    n = 0
    for i in seq:
        regs = [r.strip(',') for r in i[1:]]
        if i[0] not in SLOW:  # a load: the register no longer holds a result
            last.pop(regs[-1] if i[0] == 'mtc1' else regs[0], None)
            continue
        for r in regs[1:]:
            if r in last: worst = min(worst, n - last[r])
        last[regs[0]] = n
        n += 1
    ops = [i for i in ops if i[0] in SLOW]
    print(f'{f[:60]:60s} {b:10s} {len(ops):3d} FPU ops, {"loop" if loop else "once"}, closest reuse {worst}')
