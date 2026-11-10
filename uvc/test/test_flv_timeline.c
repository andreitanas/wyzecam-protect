// Mux sessions must share capture time across channel changes/reconnects.
// Link with flv, h264 and util, without audio.o: this stub records lookups.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "flv.h"

#define FRAME_MS 64  // AAC-LC, 16 kHz, 1024 samples

static int64_t queries[16];
static size_t query_count;

size_t audio_frame_next(int64_t *next, int64_t t_ms, uint8_t *out) {
    static const uint8_t silence[] = {0x01, 0x18, 0x20, 0x07};
    // Catch accidental startup backfill promptly, rather than building a
    // huge stream if a late connection starts audio at timestamp zero.
    assert(query_count < sizeof(queries) / sizeof(queries[0]));
    queries[query_count++] = t_ms;
    memcpy(out, silence, sizeof(silence));
    ++*next;
    return sizeof(silence);
}

static uint32_t u24(const uint8_t *p) {
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] << 24 | u24(p + 1); }

struct observed {
    size_t audio_count;
    uint32_t audio_ts[4];
    uint32_t video_ts;
    uint32_t audio_config_ts, video_config_ts;
    int audio_config, video_config;
    uint32_t first_wire, first_ts, base;
};

// The Opus track is not what these tests are about: always silence
size_t audio_opus_next(int64_t *next, int64_t t_ms, uint8_t *out) {
    ++*next;
    return 0;
}

static struct observed inspect(const struct buf *out) {
    struct observed got = {0};
    size_t pos = out->len >= 3 && !memcmp(out->p, "FLV", 3) ? 13 : 0;
    int saw_av = 0;
    uint32_t last_av = 0;
    while (pos < out->len) {
        assert(pos + 31 <= out->len);
        const uint8_t *p = out->p + pos;
        unsigned type = p[0];
        size_t size = u24(p + 1);
        uint32_t wire = u24(p + 4) | (uint32_t)p[7] << 24;
        assert(pos + 31 + size <= out->len);
        assert(u32(p + 11 + size) == size + 11);
        // The shared timeline is in the trailer's ticks; the tag's own
        // timestamp counts from the connection's first tag
        const uint8_t *tr = p + 15 + size;
        uint32_t clock = u24(tr + 1);
        assert(clock == (type == 8 ? 16000u : type == 10 ? 48000u : 90000u));
        assert(u32(tr + 4) == 0);
        uint64_t ticks = (uint64_t)u32(tr + 8) << 32 | u32(tr + 12);
        uint32_t ts = (uint32_t)(ticks / (clock / 1000));
        if (pos <= 13)
            got.first_wire = wire, got.first_ts = ts;
        assert(ts - wire == got.first_ts - got.first_wire || pos <= 13);
        got.base = ts - wire;
        if (type == 8 || type == 9) {
            assert(size >= 2);
            assert(!saw_av || ts >= last_av);
            saw_av = 1;
            last_av = ts;
            if (type == 8) {
                if (p[12] == 0) {
                    got.audio_config = 1;
                    got.audio_config_ts = ts;
                } else {
                    assert(got.audio_count < sizeof(got.audio_ts) / sizeof(got.audio_ts[0]));
                    got.audio_ts[got.audio_count++] = ts;
                }
            } else if (p[12] == 0) {
                got.video_config = 1;
                got.video_config_ts = ts;
            } else {
                got.video_ts = ts;
            }
        }
        pos += 31 + size;
    }
    assert(pos == out->len);
    return got;
}

static struct observed frame(struct flv *mux, int64_t origin_ms, int64_t capture_ms) {
    static const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0x80};
    query_count = 0;
    flv_video(mux, capture_ms - origin_ms, 1, idr, sizeof(idr));
    struct observed got = inspect(&mux->out);
    assert(got.video_ts == (uint32_t)(capture_ms - origin_ms));
    assert(got.audio_count == query_count);
    for (size_t i = 0; i < got.audio_count; i++) {
        assert(got.audio_ts[i] % FRAME_MS == 0);
        assert(queries[i] == origin_ms + got.audio_ts[i]);
        assert(queries[i] <= capture_ms);
    }
    mux->out.len = 0;
    return got;
}

static struct observed begin(struct flv *mux, int64_t origin_ms, int64_t capture_ms) {
    static const uint8_t sps[] = {0x67, 0x4d, 0x40, 0x1e};
    static const uint8_t pps[] = {0x68, 0xee, 0x3c, 0x80};
    flv_begin(mux, "timeline", 1, 640, 360, 25, 1, origin_ms, capture_ms);
    flv_avc_config(mux, sps, sizeof(sps), pps, sizeof(pps));
    struct observed got = frame(mux, origin_ms, capture_ms);
    uint32_t first_audio = (uint32_t)((capture_ms - origin_ms) / FRAME_MS * FRAME_MS);
    assert(got.audio_count >= 1 && got.audio_count <= 2);
    assert(got.audio_ts[0] == first_audio);
    assert(got.audio_config && got.video_config);
    assert(got.audio_config_ts == first_audio);
    assert(got.video_config_ts == first_audio);
    assert(got.video_config_ts <= got.video_ts);
    // The connection's tags start at zero and all share one base
    assert(got.first_wire == 0 && got.base == first_audio);
    return got;
}

int main(void) {
    struct flv high, medium, reopened, shifted;
    begin(&high, 0, 600000);

    // High quality has run for a minute before medium quality is opened.
    // Advance actual frames so this is a channel join, not a capture gap.
    struct observed high_join = {0};
    for (int64_t capture = 600040; capture <= 660000; capture += 40)
        high_join = frame(&high, 0, capture);
    struct observed medium_join = begin(&medium, 0, 660000);
    assert(high_join.video_ts == medium_join.video_ts);
    assert(high_join.audio_count == 1 && medium_join.audio_count == 1);
    assert(high_join.audio_ts[0] == medium_join.audio_ts[0]);

    struct observed high_next = frame(&high, 0, 660040);
    struct observed medium_next = frame(&medium, 0, 660040);
    assert(high_next.video_ts == medium_next.video_ts);
    assert(high_next.audio_count == medium_next.audio_count);
    for (size_t i = 0; i < high_next.audio_count; i++)
        assert(high_next.audio_ts[i] == medium_next.audio_ts[i]);

    // A fresh connection after ten more minutes keeps time and emits only
    // current audio. The test's bounded stub rejects historical backfill.
    struct observed reconnect = begin(&reopened, 0, 1260040);
    assert(reconnect.video_ts > high_next.video_ts);

    // Nonzero origins verify that audio lookup uses capture time, rather
    // than accidentally using the session-relative tag timestamp alone.
    struct observed shifted_start = begin(&shifted, 123456, 123456 + 660000);
    assert(shifted_start.video_ts == medium_join.video_ts);
    assert(shifted_start.audio_ts[0] == medium_join.audio_ts[0]);

    // FLV's wire milliseconds wrap after 49 days. Internal capture time
    // must stay wide, or microphone lookups would jump back 49 days.
    struct flv long_uptime;
    int64_t late = (INT64_C(1) << 32) + 660040;
    static const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0x80};
    flv_begin(&long_uptime, "longUptime", 1, 640, 360, 25, 1, 0, late);
    query_count = 0;
    flv_video(&long_uptime, late, 1, idr, sizeof(idr));
    struct observed wrapped = inspect(&long_uptime.out);
    assert(wrapped.video_ts == (uint32_t)late);
    assert(query_count == 1 && queries[0] == late / FRAME_MS * FRAME_MS);
    flv_free(&long_uptime);

    flv_free(&high);
    flv_free(&medium);
    flv_free(&reopened);
    flv_free(&shifted);
    puts("FLV timeline: channel joins, reconnects, AAC alignment and bounded startup passed");
    return 0;
}
