#!/bin/sh
# Build the camera's busybox, static mipsel against musl:
# build/busybox/busybox, with build/busybox/busybox.links listing its
# applets' paths (image/build-rootfs.py links them).
# busybox's defconfig, less what a camera has no use for or cannot run.
set -e
cd "$(dirname "$0")/.."
SRC=$PWD/third_party/busybox
OUT=$PWD/build/busybox
[ -d "$SRC" ] || ./fetch-deps.sh
mkdir -p "$OUT"

# zig cc without the GNU ld options zig's linker does not know (busybox's
# link script passes them; they only sort sections and write a map)
cat > "$OUT/zcc" <<'SH'
#!/bin/sh
for a; do
    shift
    case "$a" in
    -Wl,--warn-common | -Wl,--sort-common | -Wl,--sort-section,* | -Wl,-Map,* | -Wl,--verbose) ;;
    *) set -- "$@" "$a" ;;
    esac
done
exec zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 "$@"
SH
chmod +x "$OUT/zcc"
MAKE="make -s -C $SRC O=$OUT HOSTCC=cc CC=$OUT/zcc AR=$PWD/tools/zig-ar SKIP_STRIP=y"

# Off: hash code for x86 only; what needs SysV IPC, which the kernel does
# not have (syslogd logs to a file, S01system); syslogd's remote logging,
# with which 1.37.0 logs nothing locally (syslogd_init sets the flag in
# option_mask32 and returns the copy made before); servers nothing here runs
OFF="SHA1_HWACCEL SHA256_HWACCEL
FEATURE_IPC_SYSLOG LOGREAD IPCS IPCRM FEATURE_REMOTE_LOG
TELNETD FTPD FTPGET FTPPUT TFTPD HTTPD INETD UDHCPD DHCPRELAY DNSD FAKEIDENTD
NBDCLIENT TC LPD LPR LPQ SENDMAIL POPMAILDIR MAKEMIME REFORMIME"
# On: static; the flash tools image/flash-modern.sh and rollback write with
ON="STATIC FLASHCP FLASH_ERASEALL FLASH_LOCK FLASH_UNLOCK"
$MAKE defconfig >/dev/null
python3 - "$OUT/.config" "$ON" $OFF <<'PY'
import re, sys
path, on, off = sys.argv[1], sys.argv[2].split(), sys.argv[3:]
s = open(path).read()
for k in on:
    s, n = re.subn(r'^# CONFIG_%s is not set$' % k, 'CONFIG_%s=y' % k, s, flags=re.M)
    assert n == 1, k
for k in off:
    s, n = re.subn(r'^CONFIG_%s=y$' % k, '# CONFIG_%s is not set' % k, s, flags=re.M)
    assert n == 1, k
open(path, 'w').write(s)
PY
yes "" | $MAKE oldconfig >/dev/null
$MAKE EXTRA_LDFLAGS=-s -j8 >/dev/null 2>&1 || $MAKE EXTRA_LDFLAGS=-s 2>&1 | tail -20
$MAKE busybox.links >/dev/null
ls -l "$OUT/busybox"
