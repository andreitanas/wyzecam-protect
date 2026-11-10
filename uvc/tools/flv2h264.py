"""The H.264 of a UniFi FLV stream captured mid-connection (tcpstreams.py's
.c2s), as Annex B from its first keyframe on (keyframes carry SPS and PPS).
usage: flv2h264.py in.c2s out.h264"""
import sys
d = open(sys.argv[1], 'rb').read()

def tag(i):  # body size if a tag with its PreviousTagSize and trailer starts at i
    if i + 15 > len(d) or d[i] not in (8, 9, 10, 18): return 0
    size = int.from_bytes(d[i+1:i+4], 'big'); e = i + 11 + size
    if e + 20 > len(d) or int.from_bytes(d[e:e+4], 'big') != size + 11 or d[e+4]: return 0
    return size

i = 0; out = bytearray(); started = False
while i < len(d) - 31:
    size = tag(i)
    if not size:
        i += 1
        continue
    body = d[i+11:i+11+size]
    if d[i] == 9 and body[1] == 1:
        started = started or body[0] >> 4 == 1
        p = 5
        while started and p + 4 <= len(body):
            n = int.from_bytes(body[p:p+4], 'big')
            out += b'\0\0\0\1' + body[p+4:p+4+n]
            p += 4 + n
    i += 11 + size + 20
open(sys.argv[2], 'wb').write(out)
