#!/bin/sh
# A picture of what the camera is sending to Protect right now: captures the
# high stream on the NVR for a few seconds and decodes its first keyframe.
#   tools/streamsnap.sh out.jpg [seconds] [camera-ip] [nvr]
set -e
cd "$(dirname "$0")/.."
OUT=$1; SECS=${2:-8}; CAM=${3:-192.168.1.9}; NVR=${4:-root@192.168.1.1}
T=$(mktemp -d)
ssh "$NVR" "timeout $SECS tcpdump -i br0 -s0 -w /tmp/snap.pcap 'host $CAM and tcp port 7550' >/dev/null 2>&1; cat /tmp/snap.pcap; rm /tmp/snap.pcap" > "$T/c.pcap"
# The connection with the most data is the high stream
C=$(python3 tools/tcpstreams.py "$T/c.pcap" | sort -k11 -n -r | head -1 | awk '{print $1}')
python3 tools/tcpstreams.py "$T/c.pcap" extract "$C" "$T/hi" >/dev/null
python3 tools/flv2h264.py "$T/hi.c2s" "$T/hi.h264"
ffmpeg -v error -y -i "$T/hi.h264" -frames:v 1 "$OUT"
rm -rf "$T"
