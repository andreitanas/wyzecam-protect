#!/usr/bin/env python3
"""Make a demo.bin, the file the Wyze bootloader flashes from the SD card
while the setup button is held, from a kernel and a root partition image.

    demo-bin.py UIMAGE ROOT.sqfs OUT.bin

demo.bin is a legacy U-Boot image (Linux, MIPS, type firmware, uncompressed,
named jz_fw) of the kernel partition (2 MiB) followed by the root
partition's first 274 blocks. Shorter inputs are padded with 0xff.
"""
import struct
import sys
import time
import zlib

KERNEL = 0x200000
ROOT = 274 * 32768


def main(uimage, root, out):
    kernel = open(uimage, 'rb').read()
    r = open(root, 'rb').read()
    if len(kernel) > KERNEL or len(r) > ROOT:
        sys.exit(f'{uimage} or {root} does not fit its partition')
    data = kernel.ljust(KERNEL, b'\xff') + r.ljust(ROOT, b'\xff')
    # magic, header CRC, time, size, load, entry, data CRC, OS, arch, type,
    # compression, name
    fields = [0x27051956, 0, int(time.time()), len(data), 0, 0, zlib.crc32(data), 5, 5, 5, 0, b'jz_fw']
    fields[1] = zlib.crc32(struct.pack('>7I4B32s', *fields))
    open(out, 'wb').write(struct.pack('>7I4B32s', *fields) + data)
    print(f'{out}: {64 + len(data)} bytes')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
