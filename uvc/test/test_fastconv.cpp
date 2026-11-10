// uvcd's convolution layers (src/fastconv.cpp) against ncnn's own: the
// detector model's raw outputs for the same input must agree, at input sizes
// that exercise the kernels' group tails and the fallbacks for tiny maps.
//   test_fastconv model   (path without .param/.bin)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "fastconv.h"
#include "net.h"

static int load(ncnn::Net &net, const char *model, bool own) {
    net.opt.num_threads = 1;
    // Full single precision in both (ncnn computes in half floats on ARM64 hosts)
    net.opt.use_fp16_packed = net.opt.use_fp16_storage = net.opt.use_fp16_arithmetic = false;
    net.opt.use_bf16_storage = false;
    if (own)
        fastconv_register(net);
    char path[256];
    snprintf(path, sizeof(path), "%s.param", model);
    if (net.load_param(path))
        return -1;
    snprintf(path, sizeof(path), "%s.bin", model);
    return net.load_model(path);
}

int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    ncnn::Net ours, theirs;
    if (load(ours, argv[1], true) || load(theirs, argv[1], false)) {
        fprintf(stderr, "cannot load %s\n", argv[1]);
        return 1;
    }
    // Blobs before the detection layer (argv[2..], default Yolo-Fastest's two heads)
    const char *def[] = {"120_854", "129_922"};
    const char **blobs = argc > 2 ? (const char **)argv + 2 : def;
    int nblobs = argc > 2 ? argc - 2 : 2;
    static const int sizes[][2] = {{320, 192}, {96, 96}, {352, 160}, {64, 224}, {160, 96}, {288, 320}};
    int failed = 0;
    unsigned seed = 1;
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int w = sizes[s][0], h = sizes[s][1];
        ncnn::Mat in(w, h, 3);
        for (int c = 0; c < 3; c++) {
            float *p = in.channel(c);
            for (int i = 0; i < w * h; i++) { // smooth blobs plus noise, 0..1
                seed = seed * 1103515245 + 12345;
                p[i] = 0.5f + 0.3f * sinf(i % w * 0.05f * (c + 1)) * cosf(i / w * 0.07f) +
                       ((seed >> 16) % 200) / 1000.f;
            }
        }
        for (int b = 0; b < nblobs; b++) {
            ncnn::Extractor a = ours.create_extractor(), r = theirs.create_extractor();
            a.input("data", in);
            r.input("data", in);
            ncnn::Mat x, y;
            if (a.extract(blobs[b], x) || r.extract(blobs[b], y) || x.w != y.w || x.h != y.h || x.c != y.c ||
                x.elempack != 1 || y.elempack != 1) {
                printf("%dx%d %s: no output or shapes differ\n", w, h, blobs[b]);
                failed = 1;
                continue;
            }
            double worst = 0, peak = 0;
            for (int c = 0; c < x.c; c++) {
                const float *p = x.channel(c), *q = y.channel(c);
                for (int i = 0; i < x.w * x.h; i++) {
                    worst = fmax(worst, fabs(p[i] - q[i]));
                    peak = fmax(peak, fabs(q[i]));
                }
            }
            int bad = !(worst <= 1e-3 * (peak > 1 ? peak : 1));
            printf("%dx%d %s (%dx%dx%d): largest difference %.2g, largest value %.3g%s\n", w, h, blobs[b],
                   x.w, x.h, x.c, worst, peak, bad ? "  FAILED" : "");
            failed |= bad;
        }
    }
    return failed;
}
