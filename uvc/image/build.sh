#!/bin/sh
# Build the camera's image:
#   build/image/uImage    for the kernel partition (mtd1), from kernel/build.sh
#   build/image/root.sqfs for the root partition (mtd2): read-only squashfs,
#                         mounted as / by the kernel
#   build/image/demo.bin  both, for the SD card and the setup button
# The root filesystem is all built here (image/build-rootfs.py): busybox,
# dropbear, wpa_supplicant, image/rootfs (init scripts, /etc), the kernel's
# modules, uvcd with its model under /rom/uvc, and the driver test tools.
# No initramfs: nothing in RAM but /tmp and the writable layer over /etc
# (S00rootfs). Settings are on the config partition (mtd3, JFFS2).
# Install with image/flash.sh over ssh, or with demo.bin.
set -e
cd "$(dirname "$0")/.."
OUT=build/image
KOUT=build/kernel/out
# demo.bin carries the root partition's first 274 blocks (image/demo-bin.py)
MAX=$((274 * 32768))
ZCC="zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2 -Os -static -s"

[ -f $KOUT/uImage ] || kernel/build.sh
make -s camera
[ -x build/busybox/busybox ] || tools/build-busybox.sh
[ -x build/dropbear/dropbearmulti ] || tools/build-dropbear.sh
[ -x build/wpa_supplicant/wpa_supplicant ] || tools/build-wpa-supplicant.sh

P=$OUT/payload
rm -rf $P
mkdir -p $P/usr/sbin $P/usr/bin $P/rom/uvc/models
cp build/dropbear/dropbearmulti $P/usr/sbin/dropbear
ln -s ../sbin/dropbear $P/usr/bin/dropbearkey
cp build/wpa_supplicant/wpa_supplicant build/wpa_supplicant/wpa_cli $P/usr/sbin/
cp build/uvcd-mipsel $P/rom/uvc/uvcd
cp third_party/models/yolo-fastest-1.1.param third_party/models/yolo-fastest-1.1.bin \
    $P/rom/uvc/models/
# Driver tests (docs/kernel-modern.md, hardware test plan), ISP registers
# and controls (docs/t20-isp-hardware.md), the microphone
for t in v4l2grab v4l2jpeg v4l2h264 isptool audiorec; do
    $ZCC -o $P/usr/bin/$t capture/$t.c
done

python3 image/build-rootfs.py $OUT/rootfs.tar build/busybox/busybox build/busybox/busybox.links \
    image/rootfs $KOUT/modules $P
# Owned by root, xz like the kernel's squashfs support
rm -f $OUT/root.sqfs
sqfstar -quiet -comp xz -b 131072 -no-xattrs -force-uid 0 -force-gid 0 \
    -mkfs-time 0 -all-time 0 $OUT/root.sqfs < $OUT/rootfs.tar
SIZE=$(wc -c < $OUT/root.sqfs)
if [ "$SIZE" -gt $MAX ]; then
    echo "root.sqfs is $SIZE bytes, more than the $MAX demo.bin carries" >&2
    exit 1
fi
cp $KOUT/uImage $OUT/uImage
python3 image/demo-bin.py $OUT/uImage $OUT/root.sqfs $OUT/demo.bin
echo "$OUT/root.sqfs: $SIZE bytes ($(( (SIZE + 32767) / 32768 )) of 274 blocks); $OUT/uImage: $(wc -c < $OUT/uImage) bytes"
