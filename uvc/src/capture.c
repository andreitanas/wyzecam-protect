#include "capture.h"

#include "audio.h"
#include "capture_v4l2.h"
#include "util.h"

// Video from our kernel's V4L2 drivers (capture_v4l2.c) and the microphone
// (audio.h), started and stopped together.
int capture_start(const struct config *c, struct source *hi, struct source *lo) {
    if (!capture_v4l2_available()) {
        log_error("capture: no camera drivers (t20-isp); CAPTURE=1 needs the camera and its kernel");
        return -1;
    }
    if (capture_v4l2_start(c, hi, lo) < 0)
        return -1;
    if (c->audio)
        audio_start(c->audio > 1);
    return 0;
}

void capture_stop(void) {
    audio_stop();
    capture_v4l2_stop();
}

int capture_jpeg(uint8_t **data, size_t *len) { return capture_v4l2_jpeg(data, len); }

int capture_raw(uint8_t *buf, size_t cap, int *w, int *h, int chroma) {
    return capture_v4l2_raw(buf, cap, w, h, chroma);
}

void capture_isp(const struct isp_params *p) { capture_v4l2_isp(p); }

void capture_night(int night) { capture_v4l2_night(night); }

int capture_exposure(void) { return capture_v4l2_exposure(); }
