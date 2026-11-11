# wyzecam-protect

Firmware that makes a Wyze Cam v2 (Ingenic T20) adopt into UniFi Protect as a
UniFi camera: video, audio, snapshots, motion and smart detections (person,
vehicle, animal), with no RTSP, ONVIF or bridge box.

- **`uvcd`**, a static C daemon, speaks Protect's own camera protocol and
  streams as UniFi's cameras do, so Protect's web and mobile apps play it.
  It runs the object detector (ncnn, Yolo-Fastest) on the camera.
- **Linux 6.18** with our drivers for the T20: clocks, pins, the image
  sensor, the ISP, the H.264 and JPEG encoders and the microphone. uvcd
  uses them through V4L2 and ALSA: no vendor kernel and no closed library.
- **The image** is a kernel and a read-only squashfs root (busybox, dropbear,
  wpa_supplicant, the modules, uvcd) in the camera's flash, all built from
  source here. Settings live in the config flash partition, so the SD card
  is optional.

This is not a product of, or endorsed by, Wyze or Ubiquiti.

## Status

It runs on one Wyze Cam v2 (JX-F22 sensor), every day, with UniFi Protect on
a UniFi OS console. `docs/kernel-modern.md` has the full state. Known gaps:

- The speaker is not driven: no talkback.
- Night mode switches the IR filter and LEDs, but its switching points have
  not been checked in a dark room yet.
- The JX-F23 sensor (later Wyze Cam v2 units) has a driver table and has not
  been run.
- Installing with the setup button, the way onto a new camera, has not
  been tried with this kernel; see Install.

## Build

Needs zig, the cross compiler for everything on the camera but the kernel
(built and tested with 0.17.0; nothing pins the version), cmake and ninja
for ncnn, python3, squashfs-tools, and for the kernel a Linux x86_64 host or
Docker (`docs/kernel-modern.md`, Building).

```sh
cd uvc
./fetch-deps.sh             # third-party sources, once
kernel/build.sh             # Linux: build/kernel/out/ (uImage, modules)
image/build.sh              # build/image/{uImage,root.sqfs,demo.bin}
```

- WiFi is WPA or WPA2 personal; WPA3-only networks are not supported.
- `./deploy.sh utils` and `./deploy.sh passwd` generate the camera's
  settings (WiFi, hostname, root password, SSH key) under
  `deploy/config/overlay`.
- `make -C uvc test` runs uvcd's tests on the host.

## Install

- **New camera, by SD card. Not tried yet with this kernel:** it worked
  with the vendor-kernel image, which used the same bootloader procedure
  and file format.
  1. Run `uvc/make-sdcard.sh /Volumes/CARD`. It builds the image and copies
     `demo.bin` with your settings. The card has nothing per camera: each
     camera names itself after its MAC (`wyze-2BE8`) and makes its own
     certificate.
  2. Hold the setup button while powering the camera on. The bootloader
     flashes `demo.bin`.
  3. Take `demo.bin` off the card once the camera is up.
- **Camera already running this firmware, over SSH. Tested:**
  `uvc/image/flash.sh [camera-ip]` saves the running image to the SD
  card, writes and verifies the kernel and root partitions, and reboots.
  - The first boots are trials. Check the camera, then run `rollback keep`
    on it within 10 minutes; otherwise it puts the saved image back.
  - It also works from a camera running the vendor-kernel image.

**Recovery:** a known-good `demo.bin` on the SD card, the setup button held
at power-on. The proven one is the vendor-kernel image (below).

**Adopting:** the camera answers Protect's discovery and shows up there as
ready to adopt. Protect's discovery only reaches its own subnet; elsewhere
set `NVR_HOST` and `ADOPTION_TOKEN` in `config/uvc/uvcd.conf` (see the
comments in `uvc/image/uvcd.conf.template`). Removing the camera in Protect
resets it to ready to adopt.

## On the camera

- SSH as root with your key (`deploy.sh passwd` installs your public key).
- uvcd logs to `/tmp/uvcd.log`, the system to `/tmp/messages`; both are in
  RAM and size-capped.
- Settings: `/config/uvc/uvcd.conf`.
- `/sdcard/uvc/uvcd`, if present, runs instead of the flashed uvcd, for
  development.
- `isptool input` says whether the camera input is in step with the sensor;
  `v4l2grab`, `v4l2jpeg` and `v4l2h264` test the drivers without uvcd.

## The vendor kernel

uvcd first ran on OpenMiko's 3.10 kernel through Ingenic's closed libimp.
That backend and its image scripts are gone from `main`; the tag
`vendor-kernel` is the last commit with them, and builds that image with
`uvc/image/build.sh` there.

## Layout and documents

- `uvc/src` uvcd; `uvc/kernel` the kernel's configuration, device tree and
  patches; `uvc/image` the image and flash scripts; `uvc/capture` driver
  test tools; `uvc/tools` scripts for looking at streams and captures.
- `docs/kernel-modern.md`: the kernel, what was ported from where, the
  camera stack, decisions with their dates, the hardware test plan.
- `docs/t20-isp-hardware.md`: the T20's camera input, ISP, encoders and
  audio as verified on hardware.
- `docs/protect-streaming.md`: the stream Protect expects from a camera.
- `docs/firmware-review.md`, `docs/kernel-and-sdk.md`: the review of
  OpenMiko and of the vendor kernel and SDK that led here.
