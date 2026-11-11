// ISP debugging on the camera: registers through /dev/mem and V4L2 controls
// by name (docs/t20-isp-hardware.md has the register map).
//   isptool r addr [words]         read physical words
//   isptool w addr value           write one
//   isptool dump                   VIC, interrupt controller and ISP core
//   isptool watch ms addr[:words]...  the same words every ms, a line a sample
//   isptool ctrl name [value]      get or set a control of any video node
//   isptool input                  what the ISP measures of its input for a
//                                  second: line and frame sizes and the
//                                  blanking between them. Even blanking means
//                                  the VIC is in step with the sensor; exits 1
//                                  if not (then the picture is shifted)
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#define BASE 0x13300000u
#define SPAN 0x100000u

static volatile uint32_t *io;

static uint32_t rd(uint32_t addr) { return io[(addr - BASE) / 4]; }

static void range(uint32_t from, uint32_t to) {
    for (uint32_t a = from; a < to; a += 16) {
        uint32_t v[4];
        for (int i = 0; i < 4; i++)
            v[i] = rd(a + 4 * i);
        if (v[0] | v[1] | v[2] | v[3])
            printf("%08x: %08x %08x %08x %08x\n", a, v[0], v[1], v[2], v[3]);
    }
}

static int ctrl(const char *name, const char *value) {
    for (int n = 0; n < 8; n++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", n);
        int fd = open(path, O_RDWR);
        if (fd < 0)
            continue;
        struct v4l2_queryctrl q = {.id = V4L2_CTRL_FLAG_NEXT_CTRL};
        while (ioctl(fd, VIDIOC_QUERYCTRL, &q) == 0) {
            if (!name)
                printf("%s: %s (%d..%d, default %d)\n", path, q.name, q.minimum, q.maximum, q.default_value);
            else if (!strcmp((char *)q.name, name)) {
                struct v4l2_control c = {.id = q.id, .value = value ? atoi(value) : 0};
                if (ioctl(fd, value ? VIDIOC_S_CTRL : VIDIOC_G_CTRL, &c) < 0) {
                    perror(path);
                    return 1;
                }
                printf("%s: %s = %d\n", path, q.name, c.value);
                return 0;
            }
            q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
        }
        close(fd);
    }
    return name ? (fprintf(stderr, "no control %s\n", name), 1) : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: isptool r addr [words] | w addr value | dump | input | watch ms addr[:words]... | ctrl [name [value]]\n");
        return 2;
    }
    if (!strcmp(argv[1], "ctrl"))
        return ctrl(argc > 2 ? argv[2] : NULL, argc > 3 ? argv[3] : NULL);
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    io = mmap(NULL, SPAN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, BASE);
    if (fd < 0 || io == MAP_FAILED) {
        perror("/dev/mem");
        return 1;
    }
    if (!strcmp(argv[1], "r") && argc > 2) {
        uint32_t a = strtoul(argv[2], NULL, 0), n = argc > 3 ? strtoul(argv[3], NULL, 0) : 1;
        for (uint32_t i = 0; i < n; i++)
            printf("%08x: %08x\n", a + 4 * i, rd(a + 4 * i));
    } else if (!strcmp(argv[1], "w") && argc > 3) {
        io[(strtoul(argv[2], NULL, 0) - BASE) / 4] = strtoul(argv[3], NULL, 0);
    } else if (!strcmp(argv[1], "dump")) {
        range(0x13300000, 0x13300100); // VIC
        range(0x13320000, 0x13320020); // interrupt controller
        range(0x13380000, 0x13380e00); // ISP core
    } else if (!strcmp(argv[1], "input")) {
        // The core's frame statistics: reset, a second of frames, read
        io[(0x13380970 - BASE) / 4] = 1;
        io[(0x13380970 - BASE) / 4] = 0;
        sleep(1);
        uint32_t v[16];
        for (int i = 0; i < 16; i++)
            v[i] = rd(0x13380980 + 4 * i);
        int ok = v[7] && v[0] == v[1] && v[4] == v[5] && v[9] - v[8] <= 2 && v[13] - v[12] <= 2;
        printf("%u frames, %u-%u x %u-%u, blanking %u-%u between lines, %u-%u between frames (ISP clocks): %s\n",
               v[7], v[0], v[1], v[4], v[5], v[8], v[9], v[12], v[13], ok ? "in step" : "OUT OF STEP");
        return !ok;
    } else if (!strcmp(argv[1], "watch") && argc > 3) {
        int ms = atoi(argv[2]);
        for (;;) {
            struct timespec t;
            clock_gettime(CLOCK_REALTIME, &t);
            printf("%ld.%03ld", (long)t.tv_sec, t.tv_nsec / 1000000);
            for (int i = 3; i < argc; i++) {
                char *end;
                uint32_t a = strtoul(argv[i], &end, 0), n = *end == ':' ? strtoul(end + 1, NULL, 0) : 1;
                for (uint32_t k = 0; k < n; k++)
                    printf(" %08x", rd(a + 4 * k));
            }
            printf("\n");
            fflush(stdout);
            usleep(ms * 1000);
        }
    } else {
        return 2;
    }
    return 0;
}
