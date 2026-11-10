#!/bin/sh
# Rebuild the bench with uvcd's convolution layers (src/fastconv.cpp) for the
# host and the camera and check the generated loops.
set -e
cd "$(dirname "$0")"
Z="zig c++ -target mipsel-linux-musleabihf -mcpu=mips32r2 -std=c++11 -fno-rtti -fno-exceptions -Isrc/ncnn/src"
F="-O3 -ffp-contract=fast -fno-math-errno -fno-trapping-math"
mkdir -p ../build/bench
c++ -std=c++11 -O2 -Ibuild-host/src -Isrc/ncnn/src -c bench.cpp -o ../build/bench/bench.o
c++ -std=c++11 $F -fno-rtti -fno-exceptions -Ibuild-host/src -Isrc/ncnn/src -c ../src/fastconv.cpp -o ../build/bench/fastconv.o
c++ ../build/bench/bench.o ../build/bench/fastconv.o build-host/src/libncnn.a -o bench-host
$Z -Ibuild-mips/src -O2 -c bench.cpp -o ../build/bench/bench-m.o
$Z -Ibuild-mips/src $F -c ../src/fastconv.cpp -o ../build/bench/fastconv-m.o
$Z -Ibuild-mips/src $F -g0 -S ../src/fastconv.cpp -o ../build/bench/fastconv.s 2>/dev/null
$Z -static -s ../build/bench/bench-m.o ../build/bench/fastconv-m.o build-mips/src/libncnn.a -o bench-mipsel
[ -f ../build/ncnn-prof/src/libncnn.a ] &&
    $Z -static -s ../build/bench/bench-m.o ../build/bench/fastconv-m.o ../build/ncnn-prof/src/libncnn.a -o ../build/bench-prof-mipsel
../tools/maddgap.py ../build/bench/fastconv.s
# Agreement with ncnn's layers is test/test_fastconv.cpp's job (make test)
