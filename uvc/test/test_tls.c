// TLS handshake cost: connects to host:port n times with uvcd's TLS setup
// (src/tls.c) and prints the CPU and wall time of each handshake and what
// was negotiated. Runs on the camera (test_tls-mipsel) against Protect:
//   test_tls-mipsel /config/uvc/client.pem 192.168.1.1 7444 5
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "tls.h"
#include "util.h"

static int64_t cpu_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s pem host port [count]\n", argv[0]);
        return 2;
    }
    int n = argc > 4 ? atoi(argv[4]) : 3;
    int64_t c0 = cpu_us();
    if (tls_init(argv[1]) < 0)
        return 1;
    printf("tls_init: %lld ms CPU\n", (long long)(cpu_us() - c0) / 1000);
    for (int i = 0; i < n; i++) {
        struct tls t;
        int64_t c = cpu_us(), w = mono_ms();
        if (tls_connect(&t, argv[2], atoi(argv[3]), 5000) < 0)
            return 1;
        printf("handshake %d: %lld ms CPU, %lld ms wall, %s, %s\n", i, (long long)(cpu_us() - c) / 1000,
               (long long)(mono_ms() - w), mbedtls_ssl_get_version(&t.ssl), mbedtls_ssl_get_ciphersuite(&t.ssl));
        tls_close(&t);
    }
    return 0;
}
