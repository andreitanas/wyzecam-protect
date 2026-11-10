// Opus encoder cost and output: encodes 16 kHz mono 20 ms frames of a test
// signal at each complexity and prints the time per second of audio, the
// packet sizes and, with an argument, the packet an all-zero frame encodes to
// (the muxer's silence frame). Runs on the camera (test_opus-mipsel) or the host.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "opus.h"

#define RATE 16000
#define FRAME 320 // 20 ms
#define SECONDS 5

static int64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); // CPU time: uvcd may be running too
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

int main(int argc, char **argv) {
    static int16_t pcm[RATE * SECONDS];
    uint32_t seed = 1;
    for (int i = 0; i < RATE * SECONDS; i++) {
        seed = seed * 1103515245 + 12345; // room noise plus a voice-like tone
        pcm[i] = (int16_t)(3000 * sinf(2 * M_PI * 220 * i / RATE) * (1 + sinf(2 * M_PI * 3 * i / RATE)) / 2 +
                           ((int)(seed >> 16) % 2000) - 1000);
    }
    // Second argument: 1 for the CELT-only encoder (no SILK), as real cameras
    // send; third: the bit rate
    int app = argc > 2 && atoi(argv[2]) ? OPUS_APPLICATION_RESTRICTED_LOWDELAY : OPUS_APPLICATION_AUDIO;
    int bitrate = argc > 3 ? atoi(argv[3]) : 32000;
    for (int complexity = 0; complexity <= 10; complexity += complexity < 4 ? 1 : 3) {
        int err;
        OpusEncoder *enc = opus_encoder_create(RATE, 1, app, &err);
        if (!enc) {
            printf("opus_encoder_create: %d\n", err);
            return 1;
        }
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrate));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity));
        opus_encoder_ctl(enc, OPUS_SET_VBR(0));
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        uint8_t out[400];
        int min = 1000, max = 0;
        int64_t t0 = now_us();
        for (int i = 0; i + FRAME <= RATE * SECONDS; i += FRAME) {
            int len = opus_encode(enc, pcm + i, FRAME, out, sizeof(out));
            if (len < 0) {
                printf("opus_encode: %d\n", len);
                return 1;
            }
            min = len < min ? len : min;
            max = len > max ? len : max;
        }
        int64_t us = now_us() - t0;
        printf("complexity %2d: %6.1f ms CPU per second of audio (%4.1f%%), packets %d-%d bytes, TOC 0x%02x\n",
               complexity, us / 1000.0 / SECONDS, us / 10000.0 / SECONDS, min, max, out[0]);
        if (argc > 1 && complexity == atoi(argv[1])) {
            static const int16_t zero[FRAME];
            for (int k = 0; k < 3; k++) { // a few frames in, the state has settled
                int len = opus_encode(enc, zero, FRAME, out, sizeof(out));
                printf("silence frame %d: %d bytes:", k, len);
                for (int j = 0; j < len; j++)
                    printf(" 0x%02x,", out[j]);
                printf("\n");
            }
        }
        opus_encoder_destroy(enc);
    }
    return 0;
}
