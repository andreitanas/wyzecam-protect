#ifndef UVC_FLV_H
#define UVC_FLV_H

#include "util.h"

// FLV muxer producing the variant UniFi Protect ingests on port 7550:
// standard FLV tags, header flags 0x07, a 16-byte wall-clock trailer after
// every tag's PreviousTagSize (90 kHz video, AAC sample-rate audio, 48 kHz
// Opus), onClockSync/onMpma script tags every second, and with audio a second
// audio track: Opus packets in tags of type 10.
struct flv {
    struct buf out; // bytes ready to send; caller drains it
    int64_t start_ms; // shared monotonic origin, independent of TCP connections
    int64_t base_ms;  // this connection's first timestamp: the tags count from it
    int64_t last_sync_ms;
    int audio;              // 1: an AAC track; 2: the Opus track as well
    uint64_t audio_samples; // samples of audio emitted so far
    int64_t audio_next;     // the microphone's next frame (audio.h)
    uint64_t opus_packets;  // Opus packets emitted so far, 20 ms each
    int64_t opus_next;      // the microphone's next Opus packet
    int64_t video_ms, audio_ms; // full capture timeline; wire counters wrap separately
};

// channel: 0 for the high quality stream, 1 medium, 2 low
void flv_begin(struct flv *f, const char *stream_name, int channel, int width, int height, int fps,
               int audio, int64_t start_ms, int64_t first_ms);
// SPS/PPS without start codes.
void flv_avc_config(struct flv *f, const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                    size_t pps_len);
// One access unit in Annex-B form. ts in ms from the shared monotonic origin
// (the tag's own timestamp counts from the connection's first frame).
void flv_video(struct flv *f, int64_t ts, int key, const uint8_t *annexb, size_t len);
// The answer to a client's latency probe (stream_reply.h): how long ago, on
// the camera's clock, the frame it names was captured, and how long frames
// take from capture to the connection (delay_ms). now_ms from the shared origin.
void flv_rtt(struct flv *f, const char *id, uint32_t rate, uint64_t time, int64_t now_ms, int delay_ms);
void flv_free(struct flv *f);

#endif
