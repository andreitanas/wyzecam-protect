#!/bin/sh
# Build the camera's WiFi supplicant, static mipsel against musl:
# build/wpa_supplicant/wpa_supplicant and wpa_cli. WPA/WPA2-PSK over nl80211
# only (the kernel has no wireless extensions), its own crypto, and
# OpenWrt's libnl-tiny for netlink.
set -e
cd "$(dirname "$0")/.."
SRC=$PWD/third_party/wpa_supplicant
NLSRC=$PWD/third_party/libnl-tiny
NL=$PWD/build/libnl-tiny
OUT=$PWD/build/wpa_supplicant
CC="zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2"
[ -d "$SRC" ] && [ -d "$NLSRC" ] || ./fetch-deps.sh

# libnl-tiny comes with a CMake file for a shared library only
mkdir -p "$NL"
if [ ! -f "$NL/libnl-tiny.a" ]; then
    for c in "$NLSRC"/*.c; do
        $CC -Os -ffunction-sections -fdata-sections -D_GNU_SOURCE \
            -I"$NLSRC/include" -c "$c" -o "$NL/$(basename "$c" .c).o"
    done
    tools/zig-ar rcs "$NL/libnl-tiny.a" "$NL"/*.o
fi

# The Makefile builds where the sources are, so they are copied
mkdir -p "$OUT"
if [ ! -d "$OUT/tree" ]; then
    mkdir "$OUT/tree"
    cp -R "$SRC/src" "$SRC/wpa_supplicant" "$OUT/tree/"
fi
cd "$OUT/tree/wpa_supplicant"
cat > .config.new <<'C'
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL_TINY=y
CONFIG_TLS=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_BACKEND=file
CONFIG_CTRL_IFACE=y
# -s: messages to the system log (S20wifi)
CONFIG_DEBUG_SYSLOG=y
# Settings written for OpenMiko (its wpa-gen.sh) have a bgscan line
CONFIG_BGSCAN_SIMPLE=y
# and "group=CCMP TKIP WEP104 WEP40", which 2.11 refuses to parse without WEP
CONFIG_WEP=y
# /dev/urandom is enough for a station; the pool is for access points
CONFIG_NO_RANDOM_POOL=y
CONFIG_NO_CONFIG_WRITE=y
CONFIG_NO_CONFIG_BLOBS=y
CONFIG_NO_RRM=y
CONFIG_NO_WMM_AC=y
CONFIG_NO_ROBUST_AV=y
C
# An unchanged .config keeps make from rebuilding everything; a changed one
# needs all of it rebuilt
if ! cmp -s .config.new .config; then
    mv .config.new .config
    make -s clean >/dev/null 2>&1 || true
fi
rm -f .config.new
make -s QUIET=1 CC="$CC" \
    EXTRA_CFLAGS="-Os -ffunction-sections -fdata-sections -D_GNU_SOURCE -DCONFIG_LIBNL20 -I$NLSRC/include" \
    LDFLAGS="-static -s -Wl,--gc-sections -L$NL" \
    wpa_supplicant wpa_cli >/dev/null 2>&1
cp wpa_supplicant wpa_cli "$OUT/"
cd "$OUT"
ls -l wpa_supplicant wpa_cli
