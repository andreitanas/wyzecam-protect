#!/bin/sh
# Per-layer times of a model on the camera (needs build/bench-prof-mipsel from
# fast.sh and build/ncnn-prof): 8 runs, the fastest of each layer.
#   prof.sh [param] [bin] [inW] [inH]          NCNN_CONV=1 prof.sh ... for ncnn's own layers
cd "$(dirname "$0")"
P=${1:-yolo-fastest-1.1-t30.param} B=${2:-yolo-fastest-1.1.bin} W=${3:-320} H=${4:-192}
SSH="ssh -o ConnectTimeout=10 -o PubkeyAcceptedAlgorithms=+ssh-rsa -o HostKeyAlgorithms=+ssh-rsa root@192.168.1.9"
$SSH 'cat > /sdcard/bench/bench-prof-mipsel' < ../build/bench-prof-mipsel
$SSH "cd /sdcard/bench; NCNN_CONV=$NCNN_CONV ./bench-prof-mipsel $P $B scene.rgb 640 360 $W $H 9 0.3 2>&1" > ../build/bench/prof.txt
python3 ../tools/profsum.py ../build/bench/prof.txt models/$P ${TOP:-8}
