# The stream to Protect, as UniFi's cameras send it

What uvcd's stream to Protect has to look like for Protect's mobile apps, found
on the Wyze Cam v2 with the modern kernel between 2026-10-28 and 2026-11-10 by
comparing it with a UniFi G5's own stream. The browser player was never
affected: it places frames by wall-clock time. The mobile apps get the
camera's clocks, forwarded by Protect, and were unusable: audio played for a
second or two and stopped (later only clicks), and going from the camera grid
to one camera showed "synchronizing" for several seconds.

Since 2026-11-10 both apps switch without the wait and play audio (user, on an
iPhone and an Android tablet).

## The causes, most important first

1. **The trailer's tick count is 64 bits, and uvcd wrote 32.** After every FLV
   tag comes a 16-byte trailer: a zero byte, the 3-byte clock rate, 4 zero
   bytes and an 8-byte tick count. uvcd wrote 8 zero bytes and 4 bytes of
   ticks. From 13 hours of uptime the 90 kHz video ticks had wrapped and
   Protect dated the video whole wraps in the past, and the 16 kHz audio (a
   wrap every 75 hours) another number of them. Its media server logged, 300
   times a minute, `InNetLiveFLVStream::FeedData FAILED! ... Video frame
   wc=..., now=..., diff=1002159148 ms` and `last audio frame wc=... exeeds
   threshold 1000 ms`: 21 wraps of video, 3 of audio. This is why audio
   worked just after October's fixes and not days later. The errors stopped
   with the first build that wrote the full count.
2. **Latency probes went unanswered.** Protect forwards each app's probe to
   the camera on the stream connection and the camera answers with an
   `onRtt` script tag ([below](#feedback-from-protect)). uvcd ignored them,
   and the apps waited. Answering them ended the "synchronizing" wait.
3. **No Opus track.** UniFi cameras send Opus next to AAC. The iOS app asks
   Protect for Opus from a camera that lists it and plays AAC otherwise, a
   path no UniFi camera puts it on (audio still died there once in several
   tries). The Android app played AAC from both a G5 and ours.
4. Smaller differences, changed to match: tag timestamps and the clock base,
   a clock sync every second instead of every 5, the metadata's fields.

Not causes, tried in October: the audio's delay behind the video (300 ms to
100 ms), 32 kHz AAC, and the H.264 SPS's frame rate (kernel patch 0016, kept:
it ends Protect's `GetFPS invalid` warnings).

## The stream

FLV over plain TCP to Protect's port 7550, one connection a channel, with
`extendedFormat`: the 16-byte trailer after every tag's PreviousTagSize.
`uvc/src/flv.c` writes it; `uvc/tools/flvdump.py --unifi` checks it, and a
G5's captured stream passes the same checks.

- **Tags.** 18 script, 9 video, 8 AAC (`af 00` + AudioSpecificConfig, then
  `af 01` + a raw frame), and **10: Opus**, one raw packet a tag with no
  header byte, after one setup tag `cf 00 03 02`. Protect reads the setup's
  last two bytes as 5 bits it skips and a 3-bit index each: the packet
  duration (3: 20 ms) and the sample rate (2: 16 kHz); it also takes a
  19-byte `OpusHead` (its `AudioCodecInfoOpus`). Without the setup it lists
  the track as `PT` (pass-through); with it as `OPUS 16.000KHz`, as a G5's.
- **Clocks.** Tag timestamps count from the connection's first frame. The
  trailer's ticks count from one clock that all of the camera's connections
  share: ticks = (streamClockBase + tag timestamp) x rate / 1000, with rate
  90000 for video and script tags, the sample rate (16000) for AAC and 48000
  for Opus. A G5's two connections carried the same ticks for the same
  frames while their tag timestamps were 25 minutes and 45 hours.
- **onClockSync**, every 1 or 2 s on a G5, every second from uvcd:
  `streamClock` (the tag's timestamp), `streamClockBase` (the shared clock at
  timestamp 0, ms) and `wallClock` (ms). A client that joins cannot place
  frames in time before one arrives. A G5's wallClock is the time of sending;
  uvcd's is the frame's capture, 100 ms earlier.
- **onMetaData**, an object: `audioBandwidth`, `audioChannels`,
  `audioFrequency`, `channelId` (0 high, 1 medium, 2 low), `extendedFormat:
  true`, `hasAudio`, `hasVideo`, `streamId` (1, 2, 4), `streamName`,
  `videoBandwidth`, `videoFps`, `videoHeight`, `videoWidth`.
- **onMpma** (bit rate statistics) with each clock sync, as before.
- **Audio.** AAC-LC, 16 kHz mono, 1024 samples a frame, as a G5's (which
  spends 64 kbit/s on each track; uvcd 32). Opus: CELT only, wideband, 20 ms
  packets, TOC byte 0xb8, the same as a G5's.

### Feedback from Protect

On the same connection, toward the camera: a big-endian u16 type, u16 length,
then the body (`uvc/src/stream_reply.c`).

| Type | Body | Meaning |
|---|---|---|
| 1 | empty | Send an IDR. Protect asks when a client joins or resumes. |
| 4 | 16 bytes | A client's latency probe: a 4-character id (`andd`, `iosd`: the Android and iOS apps), a clock rate (1000) and the time of a video frame the client received, on the camera's shared clock. |
| 0 | 12 bytes | Receiver statistics, once a second. Not used. |
| 2, 5 | 3, 2 bytes | Not known. |

A G5 answers a probe within 20 ms with a script tag `onRtt`: `{encDelay: 50,
id: 'andd', rttMillis: 94}`. rttMillis is the shared clock's time now minus
the probe's time, encDelay the camera's delay from capture to sending.

## Decisions

- **Decision (2026-11-09): tag timestamps count from each connection's first
  frame, and onClockSync carries `streamClockBase`,** as on the G5. October's
  shared timeline (frames dated by capture time since boot on every channel
  and across reconnects) stays, in the ticks.
- **Decision (2026-11-10): uvcd sends the Opus track by default** (`AUDIO=2`
  in uvcd.conf; `AUDIO=1` is AAC only, and then `audioCodecs` lists only
  `aac`: with `opus` listed and no usable track, clients asked for Opus and
  got no audio). libopus 1.5.2, fixed point, -O3, CELT only, complexity 0,
  32 kbit/s: 3.0% of the CPU (`uvc/test/test_opus.c` on the camera; 4.6% at
  -O2 and complexity 2; a lower bit rate saves little, 12 kbit/s a fifth;
  the float build is slower; with SILK allowed it is 18% at complexity 0 and
  47% at 2). uvcd as a whole went from about 19% to about 23%.
- **Keyframes on request** (2026-10-29): uvcd asks the encoder for an IDR on
  Protect's type-1 message and when a connection starts (55-73 ms, 28-45 ms).
- Video is held until 100 ms after its capture, so that the microphone's
  frames for that time are encoded and both tracks leave in timestamp order.

## How to look at it again

- **Protect's media server** logs to `/srv/ms/logs/ms.00.log` and
  `ms.err.00.log` on the NVR. It names streams `<MAC>_0` (high), `_1`, `_2`.
  Useful lines: `AudioStreamCapabilitiesChanged` (what it made of each audio
  track), `OutNetUBNTStream::GetInstance ... audioCodecType=` (what a mobile
  client asked for), `FeedData` errors (clocks).
- **A camera's stream**: `tcpdump -i br0 -s0 -w f.pcap "host <camera> and tcp
  port 7550"` on the NVR. `uvc/tools/tcpstreams.py` lists and extracts the
  connections, `uvc/tools/flvscan.py` reads one from mid-stream. A real
  camera's connections stay open; restart it from Protect to see a start.
- **The media server's code**: `/usr/bin/ms` (aarch64, stripped) in Ghidra;
  `uvc/tools/ghidra/DecompileByString.java` decompiles the functions that
  reference a message.
- **The Android app**: `adb logcat` (its decoders and AudioTrack; four
  underruns at the start of playback are normal, a G5 shows them too),
  `adb shell screenrecord` for a switch.
- Protect's database (`psql -p 5433 -d unifi-protect`, as postgres, on the
  NVR) has what it stored of a camera's `featureFlags`.
- `make -C uvc test` covers the tick rule, the Opus track, the probe parser,
  timeline joins and reconnects, and the video payloads against ffmpeg.
- The captures of 2026-11-09, the media server and its Ghidra project are in
  the ignored `uvc/build/av-debug/2026-11-09-mobile-audio/` (NOTES.md there).

## Open

- uvcd with all of this runs from `/sdcard/uvc/uvcd` on the test camera (since
  2026-11-10 07:10 UTC); the flashed image has none of it, nor kernel patch
  0016 (loaded once by hand in October). The next image carries both.
- A longer run on the iPhone: its audio died once in several tries before the
  Opus track, and has not been watched for long since.
- Only `andd` probes were seen being answered on the wire; no `iosd` probe
  arrived in the 12 s looked at.
- Not followed up: in a capture on the NVR, one TCP flow between the camera
  (port 2058) and 64.6.119.138 port 7550, 159 KB toward the camera.
- Protect showed "Last Motion: 11 Days Ago" for this camera on 2026-11-09:
  the detector was off (`DETECT_MODEL=`), and until that day Protect dated
  the camera's frames days in the past. The detector is on again since
  2026-11-10 and sends its events (`kernel-modern.md`, Object detector), and
  the app shows its detections (user, 2026-11-10).
- Temporal noise reduction is still off in uvcd.
