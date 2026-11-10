#include "stream.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "audio.h"
#include "flv.h"
#include "h264.h"
#include "stream_reply.h"
#include "util.h"

static int send_buf(int fd, struct buf *b) {
    if (b->len && write_all(fd, b->p, b->len) < 0)
        return -1;
    b->len = 0;
    return 0;
}

// Feedback includes receiver stats and latency reports as well as IDR
// requests. Keep parser state across reads: TCP boundaries are arbitrary.
// flv: the connection's muxer once it has started, for the answers to
// latency probes; delay_ms: the last frame's time from capture to here
static void log_replies(struct stream *s, int fd, struct stream_reply *replies, struct flv *flv,
                        int delay_ms) {
    uint8_t in[256];
    ssize_t n = recv(fd, in, sizeof(in), MSG_DONTWAIT);
    if (n <= 0)
        return;
    if (stream_reply_feed(replies, in, (size_t)n)) {
        source_request_keyframe(s->src);
        log_info("%s: Protect requested a keyframe", s->channel);
    }
    for (unsigned i = 0; flv && i < replies->nprobes; i++) {
        const struct stream_probe *p = &replies->probes[i];
        flv_rtt(flv, p->id, p->rate, p->time, mono_ms(), delay_ms);
    }
    char text[sizeof(in) * 4 + 1];
    size_t k = 0;
    for (ssize_t i = 0; i < n; i++)
        k += snprintf(text + k, sizeof(text) - k, in[i] >= 0x20 && in[i] < 0x7f ? "%c" : "\\x%02x", in[i]);
    log_debug("%s: Protect sent %zd bytes: %s", s->channel, n, text);
}

// Sends one connection's worth of stream; returns when the socket fails or
// the stream is stopped.
static void session(struct stream *s, int fd) {
    struct flv flv;
    struct stream_reply replies = {0};
    int started = 0;
    int64_t connected_ms = mono_ms();
    int64_t previous_ts = 0;
    int64_t health_ms = mono_ms();
    int64_t max_gap = 0, max_age = 0, max_send = 0;
    unsigned frames = 0;

    queue_init(&s->q);
    source_subscribe(s->src, &s->q);
    while (!s->stop) {
        struct frame *f = queue_pop(&s->q, 500);
        if (!f) {
            if (started && mono_ms() - s->src->last_frame_ms > 10000) {
                log_warn("%s: no frames from %s for 10 s", s->channel, s->src->path);
                break;
            }
            continue;
        }
        if (!started) {
            // The first frame is always a keyframe (queue_push guarantees
            // it); Ingenic keyframes carry SPS/PPS in band.
            const uint8_t *sps = NULL, *pps = NULL, *nal;
            size_t sps_len = 0, pps_len = 0, pos = 0, n;
            while (h264_next_nal(f->data, f->len, &pos, &nal, &n)) {
                if (n && h264_type(nal) == NAL_SPS)
                    sps = nal, sps_len = n;
                else if (n && h264_type(nal) == NAL_PPS)
                    pps = nal, pps_len = n;
            }
            if (!sps || !pps) {
                frame_unref(f);
                continue;
            }
            // Capture timestamps already use CLOCK_MONOTONIC. Keep that
            // boot timeline across every channel and TCP reconnect, so a
            // mobile view switch cannot jump back to a new stream's zero.
            flv_begin(&flv, s->name, s->channel[5] - '1', s->src->width, s->src->height, s->fps,
                      s->audio, 0, f->ts_ms);
            flv_avc_config(&flv, sps, sps_len, pps, pps_len);
            started = 1;
            log_info("%s: streaming %s to %s:%d, first keyframe after %lld ms", s->channel,
                     s->name, s->host, s->port, (long long)(mono_ms() - connected_ms));
        }
        int64_t gap = previous_ts ? f->ts_ms - previous_ts : 0;
        int64_t age = mono_ms() - f->ts_ms;
        if (previous_ts && (gap <= 0 || gap > 200))
            log_warn("%s: video timestamp step %lld ms, frame age %lld ms", s->channel,
                     (long long)gap, (long long)age);
        previous_ts = f->ts_ms;
        if (gap > max_gap)
            max_gap = gap;
        if (age > max_age)
            max_age = age;
        frames++;
        // Give the microphone time to encode this capture interval before
        // sending either track, so audio does not arrive behind newer video.
        int64_t wait_ms = f->ts_ms + AUDIO_LATENCY_MS - mono_ms();
        if (s->audio && wait_ms > 0)
            sleep_ms((int)wait_ms);
        flv_video(&flv, f->ts_ms, f->key, f->data, f->len);
        int delay = (int)(mono_ms() - f->ts_ms); // capture to the connection
        frame_unref(f);
        int64_t send_start = mono_ms();
        if (send_buf(fd, &flv.out) < 0) {
            log_warn("%s: connection to %s:%d lost", s->channel, s->host, s->port);
            break;
        }
        int64_t send_ms = mono_ms() - send_start;
        if (send_ms > max_send)
            max_send = send_ms;
        if (send_ms > 200)
            log_warn("%s: stream write took %lld ms", s->channel, (long long)send_ms);
        if (mono_ms() - health_ms >= 60000) {
            log_info("%s: %u frames in %lld ms, max timestamp step/age/write %lld/%lld/%lld ms",
                     s->channel, frames, (long long)(mono_ms() - health_ms),
                     (long long)max_gap, (long long)max_age, (long long)max_send);
            health_ms = mono_ms();
            frames = 0;
            max_gap = max_age = max_send = 0;
        }
        log_replies(s, fd, &replies, started ? &flv : NULL, delay);
    }
    source_unsubscribe(s->src, &s->q);
    queue_destroy(&s->q);
    if (started)
        flv_free(&flv);
}

static void *stream_main(void *arg) {
    struct stream *s = arg;
    int backoff = 500;
    while (!s->stop) {
        int fd = tcp_connect(s->host, s->port, 5000);
        if (fd < 0) {
            log_warn("%s: cannot connect to %s:%d", s->channel, s->host, s->port);
        } else {
            struct timeval tv = {5, 0};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            int64_t t0 = mono_ms();
            session(s, fd);
            close(fd);
            if (mono_ms() - t0 > 30000)
                backoff = 500;
        }
        for (int waited = 0; waited < backoff && !s->stop; waited += 100)
            sleep_ms(100);
        backoff = backoff < 8000 ? backoff * 2 : 8000;
    }
    return NULL;
}

void stream_start(struct stream *s, const char *name, const char *host, int port) {
    if (s->running)
        stream_stop(s);
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->host, sizeof(s->host), "%s", host);
    s->port = port;
    s->stop = 0;
    s->running = pthread_create(&s->thread, NULL, stream_main, s) == 0;
}

void stream_stop(struct stream *s) {
    if (!s->running)
        return;
    log_info("%s: stopping", s->channel);
    s->stop = 1;
    pthread_join(s->thread, NULL);
    s->running = 0;
}

int stream_matches(const struct stream *s, const char *name, const char *host, int port) {
    return s->running && !strcmp(s->name, name) && !strcmp(s->host, host) && s->port == port;
}
