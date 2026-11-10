#include "flv.h"

#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "h264.h"

#define TAG_AUDIO 8
#define TAG_VIDEO 9
#define TAG_OPUS 10 // UniFi's: one raw Opus packet a tag, no header byte
#define TAG_SCRIPT 18

// UniFi cameras send it every 1 or 2 s. A mobile client that joins waits for
// the next one before it plays (at 5 s the apps showed "synchronizing").
#define SYNC_INTERVAL_MS 1000
#define AUDIO_RATE 16000
#define AAC_FRAME_SAMPLES 1024
#define OPUS_CLOCK 48000 // Opus's timestamp clock whatever the sample rate

// AAC-LC, 16 kHz, mono; and the silent raw frame ffmpeg's encoder emits,
// for the times the microphone has nothing (audio.h).
static const uint8_t aac_config[] = {0x14, 0x08};
static const uint8_t aac_silence[] = {0x01, 0x18, 0x20, 0x07};
// CELT, wideband, 20 ms, mono (the TOC byte the encoder in audio.c and
// UniFi's cameras produce), with CELT's silence flag
static const uint8_t opus_silence[] = {0xb8, 0xff, 0xfe};

static void amf_key(struct buf *b, const char *key) {
    size_t n = strlen(key);
    buf_u16(b, n);
    buf_put(b, key, n);
}

static void amf_num(struct buf *b, const char *key, double v) {
    amf_key(b, key);
    buf_u8(b, 0x00);
    buf_f64(b, v);
}

static void amf_bool(struct buf *b, const char *key, int v) {
    amf_key(b, key);
    buf_u8(b, 0x01);
    buf_u8(b, v ? 1 : 0);
}

static void amf_str(struct buf *b, const char *key, const char *v) {
    amf_key(b, key);
    buf_u8(b, 0x02);
    amf_key(b, v);
}

static void amf_end(struct buf *b) { buf_u24(b, 9); }

// UniFi's inter-tag trailer: a zero byte, the 3-byte clock rate (90000 for
// video and script tags, the AAC sample rate for audio, 48000 for Opus), 4
// zero bytes, then the frame's capture time in ticks of that clock, 64 bits.
//
// As UniFi's cameras do it (a G5's streams, 2026-11-09): tag timestamps count
// from the connection's first frame, the ticks on every track from one clock
// that all connections share, ticks = (base + tag timestamp) * rate / 1000,
// and onClockSync gives that base as streamClockBase. Protect dates every
// frame by its ticks. With only their low 32 bits here, as until 2026-11-09,
// a camera up for more than 13 hours had its video dated whole wraps in the
// past and its audio another number of them (16 kHz wraps every 75 h), and
// Protect's mobile clients, which get these clocks, played clicks for audio.
// Both tracks wait for microphone encoding and may leave in bursts, so the
// time is the frame's capture, not when it is sent.
static void trailer(struct flv *f, uint32_t clock, int64_t ms) {
    uint8_t t[16] = {0};
    uint64_t ticks = (uint64_t)(ms > 0 ? ms : 0) * clock / 1000;
    put_u24(t + 1, clock);
    put_u32(t + 8, (uint32_t)(ticks >> 32));
    put_u32(t + 12, (uint32_t)ticks);
    buf_put(&f->out, t, sizeof(t));
}

// ts: ms from the shared origin, not before base_ms
static void tag(struct flv *f, int type, int64_t ts, const struct buf *body, int64_t now) {
    struct buf *o = &f->out;
    uint32_t wire = (uint32_t)(ts - f->base_ms);
    buf_reserve(o, 11 + body->len + 4 + 16);
    buf_u8(o, type);
    buf_u24(o, body->len);
    buf_u24(o, wire & 0xffffff);
    buf_u8(o, wire >> 24);
    buf_u24(o, 0);
    buf_put(o, body->p, body->len);
    buf_u32(o, body->len + 11);
    if (type == TAG_VIDEO)
        f->video_ms = ts;
    else if (type == TAG_AUDIO)
        f->audio_ms = ts;
    if (type == TAG_AUDIO)
        trailer(f, AUDIO_RATE, ts);
    else if (type == TAG_OPUS)
        trailer(f, OPUS_CLOCK, ts);
    else
        trailer(f, 90000, ts);
}

static void script_name(struct buf *b, const char *name) {
    buf_u8(b, 0x02);
    amf_key(b, name);
}

static void bitrate_range(struct buf *b, const char *key) {
    amf_key(b, key);
    buf_u8(b, 0x03);
    amf_num(b, "cur", 1500000);
    amf_num(b, "max", 1500000);
    amf_num(b, "min", 1500000);
    amf_end(b);
}

// onClockSync ties the stream clock to wall time; onMpma carries bitrate
// stats a real camera reports. Same cadence and values as the proven
// unifi-cam-proxy clock_sync helper.
static void clock_sync(struct flv *f, int64_t ts, int64_t now) {
    struct buf b = {0};
    script_name(&b, "onClockSync");
    buf_u8(&b, 0x03);
    amf_num(&b, "streamClock", (uint32_t)(ts - f->base_ms));
    amf_num(&b, "streamClockBase", (double)f->base_ms);
    amf_num(&b, "wallClock", (double)(wall_ms() - (now - f->start_ms - ts)));
    amf_end(&b);
    tag(f, TAG_SCRIPT, ts, &b, now);

    b.len = 0;
    script_name(&b, "onMpma");
    buf_u8(&b, 0x03);
    bitrate_range(&b, "cs");
    bitrate_range(&b, "m");
    amf_num(&b, "r", 0);
    bitrate_range(&b, "sp");
    amf_num(&b, "t", 75000.0);
    amf_end(&b);
    tag(f, TAG_SCRIPT, ts, &b, now);
    buf_free(&b);
}

void flv_begin(struct flv *f, const char *stream_name, int channel, int width, int height, int fps,
               int audio, int64_t start_ms, int64_t first_ms) {
    memset(f, 0, sizeof(*f));
    f->start_ms = start_ms;
    f->audio_next = -2;
    // Start at this connection's first capture, on the shared AAC lattice.
    // Never backfill audio since the origin when a channel starts late.
    uint64_t elapsed = first_ms > start_ms ? (uint64_t)(first_ms - start_ms) : 0;
    f->audio_samples = elapsed * AUDIO_RATE / 1000 / AAC_FRAME_SAMPLES * AAC_FRAME_SAMPLES;
    int64_t initial_ts = audio ? (int64_t)(f->audio_samples * 1000 / AUDIO_RATE)
                              : (int64_t)elapsed;
    f->video_ms = f->audio_ms = f->base_ms = initial_ts;
    // Opus from the first packet that does not start before that
    f->opus_next = -2;
    f->opus_packets = ((uint64_t)initial_ts + AUDIO_OPUS_MS - 1) / AUDIO_OPUS_MS;
    // Publish the wall mapping immediately, including after a reconnect.
    f->last_sync_ms = first_ms - SYNC_INTERVAL_MS;
    f->audio = audio;

    static const uint8_t header[] = {'F', 'L', 'V', 1, 0x07, 0, 0, 0, 9, 0, 0, 0, 0};
    buf_put(&f->out, header, sizeof(header));

    // The fields and order of a UniFi camera's (a G5, 2026-11-09)
    struct buf b = {0};
    script_name(&b, "onMetaData");
    buf_u8(&b, 0x03);
    if (audio) {
        amf_num(&b, "audioBandwidth", 32000);
        amf_num(&b, "audioChannels", 1);
        amf_num(&b, "audioFrequency", AUDIO_RATE);
    }
    amf_num(&b, "channelId", channel);
    amf_bool(&b, "extendedFormat", 1);
    amf_bool(&b, "hasAudio", audio != 0);
    amf_bool(&b, "hasVideo", 1);
    amf_num(&b, "streamId", 1 << channel);
    amf_str(&b, "streamName", stream_name);
    amf_num(&b, "videoBandwidth", 0);
    amf_num(&b, "videoFps", fps);
    amf_num(&b, "videoHeight", height);
    amf_num(&b, "videoWidth", width);
    amf_end(&b);
    int64_t now = mono_ms();
    tag(f, TAG_SCRIPT, initial_ts, &b, now);
    buf_free(&b);
}

void flv_avc_config(struct flv *f, const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                    size_t pps_len) {
    struct buf b = {0};
    static const uint8_t hdr[] = {0x17, 0, 0, 0, 0};
    buf_put(&b, hdr, sizeof(hdr));
    buf_u8(&b, 1);
    buf_put(&b, sps + 1, 3); // profile, compatibility, level
    buf_u8(&b, 0xff);        // 4-byte NAL lengths
    buf_u8(&b, 0xe1);        // one SPS
    buf_u16(&b, sps_len);
    buf_put(&b, sps, sps_len);
    buf_u8(&b, 1);
    buf_u16(&b, pps_len);
    buf_put(&b, pps, pps_len);
    int64_t now = mono_ms();
    tag(f, TAG_VIDEO, f->video_ms, &b, now);

    if (f->audio) {
        b.len = 0;
        buf_u8(&b, 0xaf);
        buf_u8(&b, 0);
        buf_put(&b, aac_config, sizeof(aac_config));
        tag(f, TAG_AUDIO, f->audio_ms, &b, now);
    }
    if (f->audio > 1) {
        // The Opus track's setup, as the camera sent it before its first
        // packet: 0xcf 0, then two bytes Protect reads as 5 bits it skips and
        // a 3-bit index each: the packet duration (3: 20 ms) and the sample
        // rate (2: 16 kHz). Without it Protect lists the track as "PT".
        static const uint8_t opus_config[] = {0xcf, 0x00, 0x03, 0x02};
        b.len = 0;
        buf_put(&b, opus_config, sizeof(opus_config));
        tag(f, TAG_OPUS, (int64_t)(f->opus_packets * AUDIO_OPUS_MS), &b, now);
    }
    buf_free(&b);
}

void flv_rtt(struct flv *f, const char *id, uint32_t rate, uint64_t time, int64_t now_ms, int delay_ms) {
    if (!rate)
        return;
    struct buf b = {0};
    script_name(&b, "onRtt");
    buf_u8(&b, 0x03);
    amf_num(&b, "encDelay", delay_ms);
    amf_str(&b, "id", id);
    amf_num(&b, "rttMillis", (double)(now_ms - (int64_t)(time * 1000 / rate)));
    amf_end(&b);
    tag(f, TAG_SCRIPT, f->video_ms, &b, mono_ms());
    buf_free(&b);
}

static void audio_until(struct flv *f, int64_t ts, int64_t now) {
    struct buf b = {0};
    uint8_t aac[AUDIO_FRAME_MAX];
    // Both tracks up to ts, in the order of their timestamps
    for (;;) {
        int64_t ats = (int64_t)(f->audio_samples * 1000 / AUDIO_RATE);
        int64_t ots = f->audio > 1 ? (int64_t)(f->opus_packets * AUDIO_OPUS_MS) : INT64_MAX;
        if (ats > ts && ots > ts)
            break;
        if (ots < ats) {
            uint8_t packet[AUDIO_OPUS_MAX];
            size_t n = audio_opus_next(&f->opus_next, f->start_ms + ots, packet);
            b.len = 0;
            if (n)
                buf_put(&b, packet, n);
            else
                buf_put(&b, opus_silence, sizeof(opus_silence));
            tag(f, TAG_OPUS, ots, &b, now);
            f->opus_packets++;
            continue;
        }
        size_t n = audio_frame_next(&f->audio_next, f->start_ms + ats, aac);
        b.len = 0;
        buf_u8(&b, 0xaf);
        buf_u8(&b, 1);
        if (n)
            buf_put(&b, aac, n);
        else
            buf_put(&b, aac_silence, sizeof(aac_silence));
        tag(f, TAG_AUDIO, ats, &b, now);
        f->audio_samples += AAC_FRAME_SAMPLES;
    }
    buf_free(&b);
}

void flv_video(struct flv *f, int64_t ts, int key, const uint8_t *annexb, size_t len) {
    int64_t now = mono_ms();
    if (now - f->last_sync_ms >= SYNC_INTERVAL_MS) {
        f->last_sync_ms = now;
        clock_sync(f, ts, now);
    }
    // The stream waits for microphone encoding before handing us video.
    // Interleave both tracks through the same capture timestamp.
    if (f->audio)
        audio_until(f, ts, now);

    struct buf b = {0};
    buf_reserve(&b, len + 64);
    buf_u8(&b, key ? 0x17 : 0x27);
    buf_u8(&b, 1);
    buf_u24(&b, 0); // composition time: the T20 encoder emits no B-frames
    size_t pos = 0, n;
    const uint8_t *nal;
    while (h264_next_nal(annexb, len, &pos, &nal, &n)) {
        if (!n || h264_type(nal) == NAL_AUD)
            continue;
        buf_u32(&b, n);
        buf_put(&b, nal, n);
    }
    tag(f, TAG_VIDEO, ts, &b, now);
    buf_free(&b);
}

void flv_free(struct flv *f) { buf_free(&f->out); }
