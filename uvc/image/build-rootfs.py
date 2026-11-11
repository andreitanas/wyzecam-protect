#!/usr/bin/env python3
"""Build the camera's root filesystem as a tar for sqfstar.

    build-rootfs.py OUT.tar BUSYBOX BUSYBOX.links OVERLAY_DIR...

Everything in it is ours: the directories and device nodes below, busybox
with a link for each of its applets, and the overlay directories' files in
order (image/rootfs, then what image/build.sh collects: dropbear,
wpa_supplicant, the kernel's modules, uvcd and its model, the test tools).
Written as a tar, never unpacked on the host, so owners, modes and device
nodes do not depend on the host's filesystem.
"""
import io
import os
import sys
import tarfile

# Mount points and the directories programs expect. /etc, /root and /var
# get a writable layer in RAM at boot (S00rootfs).
DIRS = ('bin sbin usr/bin usr/sbin lib dev dev/pts dev/shm proc sys tmp run root '
        'config sdcard etc var').split()
MODES = {'tmp': 0o1777, 'root': 0o700}
LINKS = {
    'etc/mtab': '/proc/mounts',
    'etc/resolv.conf': '../tmp/resolv.conf',  # written by udhcpc's script
    'var/log': '../tmp',
    'var/run': '../run',
    'var/lock': '../tmp',
    'var/tmp': '../tmp',
}
# The kernel mounts devtmpfs over /dev before init; these are for the case
# that it cannot.
NODES = {'dev/console': (5, 1, 0o600), 'dev/null': (1, 3, 0o666)}


def main(out, busybox, links, *overlays):
    entries = {}  # name -> (TarInfo, data or None), later ones replace

    def add(name, type=tarfile.REGTYPE, mode=0o644, data=None, link=''):
        t = tarfile.TarInfo('./' + name)
        t.type, t.mode, t.linkname = type, mode, link
        t.size = len(data) if data is not None else 0
        entries[name] = (t, data)
        return t

    for d in DIRS:
        add(d, tarfile.DIRTYPE, MODES.get(d, 0o755))
    for name, target in LINKS.items():
        add(name, tarfile.SYMTYPE, 0o777, link=target)
    for name, (major, minor, mode) in NODES.items():
        t = add(name, tarfile.CHRTYPE, mode)
        t.devmajor, t.devminor = major, minor

    add('bin/busybox', mode=0o755, data=open(busybox, 'rb').read())
    for path in open(links).read().split():
        name = path.lstrip('/')
        add(name, tarfile.SYMTYPE, 0o777, link=os.path.relpath('bin/busybox', os.path.dirname(name)))

    for top in overlays:
        for root, dirs, files in os.walk(top):
            for f in sorted(dirs + files):
                path = os.path.join(root, f)
                name = os.path.relpath(path, top)
                if os.path.islink(path):
                    add(name, tarfile.SYMTYPE, 0o777, link=os.readlink(path))
                elif os.path.isdir(path):
                    add(name, tarfile.DIRTYPE, 0o755)
                else:
                    mode = 0o755 if os.access(path, os.X_OK) else 0o644
                    add(name, mode=mode, data=open(path, 'rb').read())

    # Parents before children, so every directory exists with our mode
    for name in list(entries):
        d = os.path.dirname(name)
        while d and d not in entries:
            add(d, tarfile.DIRTYPE, 0o755)
            d = os.path.dirname(d)
    with tarfile.open(out, 'w', format=tarfile.GNU_FORMAT) as dst:
        for name in sorted(entries):
            t, data = entries[name]
            dst.addfile(t, io.BytesIO(data) if data is not None else None)
    size = sum(t.size for t, _ in entries.values())
    print(f'{out}: {len(entries)} entries, {size / 1048576:.1f} MiB')


if __name__ == '__main__':
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
