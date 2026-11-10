"""Summarize and extract TCP streams from a pcap (LINUX_SLL2 or Ethernet), no deps.
usage: tcpstreams.py file.pcap            -> list connections
       tcpstreams.py file.pcap extract <client-ip:port> <outprefix>
Writes <outprefix>.c2s and <outprefix>.s2c (seq-ordered, duplicates dropped)."""
import struct, sys
from collections import defaultdict

def packets(path):
    with open(path, 'rb') as f:
        gh = f.read(24)
        magic = struct.unpack('<I', gh[:4])[0]
        if magic == 0xa1b2c3d4: e, scale = '<', 1e-6
        elif magic == 0xa1b23c4d: e, scale = '<', 1e-9
        elif magic == 0xd4c3b2a1: e, scale = '>', 1e-6
        else: raise SystemExit(f'not pcap: {magic:x}')
        linktype = struct.unpack(e + 'I', gh[20:24])[0]
        while True:
            ph = f.read(16)
            if len(ph) < 16: return
            ts, tsf, caplen, _ = struct.unpack(e + 'IIII', ph)
            data = f.read(caplen)
            if linktype == 276:   # LINUX_SLL2
                proto = struct.unpack('>H', data[:2])[0]; ifidx = struct.unpack('>I', data[4:8])[0]; off = 20
            elif linktype == 113: # LINUX_SLL
                proto = struct.unpack('>H', data[14:16])[0]; ifidx = 0; off = 16
            elif linktype == 1:
                proto = struct.unpack('>H', data[12:14])[0]; ifidx = 0; off = 14
            else: raise SystemExit(f'linktype {linktype}')
            if proto != 0x0800: continue
            ip = data[off:]
            ihl = (ip[0] & 15) * 4
            if ip[9] != 6: continue
            tlen = struct.unpack('>H', ip[2:4])[0]
            src = '.'.join(map(str, ip[12:16])); dst = '.'.join(map(str, ip[16:20]))
            if tlen == 0 or tlen > len(ip): tlen = len(ip)  # offloaded segments
            tcp = ip[ihl:tlen]
            if len(tcp) < 20: continue
            sp, dp, seq, ack, offflags = struct.unpack('>HHIIH', tcp[:14])
            doff = (offflags >> 12) * 4; flags = offflags & 0x1ff
            yield ts + tsf * scale, ifidx, src, sp, dst, dp, seq, flags, tcp[doff:]

def key(src, sp, dst, dp):
    return (src, sp) if dp == 7550 else (dst, dp)

if len(sys.argv) < 3:
    conns = {}
    for t, ifidx, src, sp, dst, dp, seq, flags, pl in packets(sys.argv[1]):
        k = key(src, sp, dst, dp)
        c = conns.setdefault(k, {'t0': t, 'syn': False, 'c2s': 0, 's2c': 0, 'fin': False, 'ifs': set(), 'server': dst if dp == 7550 else src})
        c['ifs'].add(ifidx)
        if flags & 0x02 and not flags & 0x10: c['syn'] = True
        if flags & 0x01: c['fin'] = True
        if dp == 7550: c['c2s'] += len(pl)
        else: c['s2c'] += len(pl)
        c['t1'] = t
    for k, c in sorted(conns.items(), key=lambda kv: kv[1]['t0']):
        print(f"{k[0]}:{k[1]:<6} -> {c['server']}  start+{c['t0']-min(x['t0'] for x in conns.values()):6.1f}s dur {c['t1']-c['t0']:6.1f}s  SYN={int(c['syn'])} FIN={int(c['fin'])}  c2s {c['c2s']:>10}  s2c {c['s2c']:>7}  ifs={sorted(c['ifs'])}")
else:
    cip, cport = sys.argv[3].split(':'); cport = int(cport); out = sys.argv[4]
    segs = {'c2s': {}, 's2c': {}}
    for t, ifidx, src, sp, dst, dp, seq, flags, pl in packets(sys.argv[1]):
        if key(src, sp, dst, dp) != (cip, cport) or not pl: continue
        d = 'c2s' if dp == 7550 else 's2c'
        segs[d].setdefault(seq, (t, pl))
    for d, m in segs.items():
        if not m: continue
        seqs = sorted(m); base = seqs[0]; buf = bytearray(); times = []
        nxt = base
        for s in seqs:
            if s < nxt:   # overlap/duplicate
                pl = m[s][1][nxt - s:]
            else:
                if s > nxt: print(f'{d}: gap of {s-nxt} bytes at {nxt-base}', file=sys.stderr); buf += b'\0' * (s - nxt)
                pl = m[s][1]
            times.append((len(buf), m[s][0])); buf += pl; nxt = s + len(m[s][1]) if s + len(m[s][1]) > nxt else nxt
        open(out + '.' + d, 'wb').write(buf)
        with open(out + '.' + d + '.times', 'w') as f:
            for off, t in times: f.write(f'{off} {t:.6f}\n')
        print(d, len(buf), 'bytes')
