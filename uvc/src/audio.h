#ifndef UVC_AUDIO_H
#define UVC_AUDIO_H

#include <stddef.h>
#include <stdint.h>

// The microphone on our kernel (/dev/dsp from the T20's internal codec),
// encoded to AAC-LC, 16 kHz mono, 1024 samples (64 ms) a frame: the stream's
// audio track (flv.c), in place of silence. 0 if it started. With opus, the
// Opus encoder runs too (6% of the T20's CPU).
int audio_start(int opus);
void audio_stop(void);

// Protect's microphone volume, 0 (silence) to 100 (the most gain)
void audio_set_volume(int volume);

// The raw AAC frame (no ADTS header) captured at monotonic time t_ms: its
// length, or 0 if there is none (not yet encoded, gone from the buffer, or
// no microphone). out holds AUDIO_FRAME_MAX bytes.
#define AUDIO_FRAME_MAX 768
size_t audio_frame_at(int64_t t_ms, uint8_t *out);
// The same for a stream of frames: the one after *next (set it to -2 to
// start), or the one at t_ms if that is more than a frame away (the
// microphone's clock drifted); *next becomes the one after it
size_t audio_frame_next(int64_t *next, int64_t t_ms, uint8_t *out);

// The second audio track: the same samples as Opus (CELT only, wideband,
// 20 ms a packet, as UniFi cameras send it). Protect's mobile apps play this
// track and its browser player the AAC one (docs/protect-streaming.md).
// Packets are numbered from the same sample 0; otherwise as audio_frame_next.
#define AUDIO_OPUS_MAX 256
#define AUDIO_OPUS_MS 20
size_t audio_opus_next(int64_t *next, int64_t t_ms, uint8_t *out);

// Allow a 64 ms microphone frame plus encoding/scheduling time before
// muxing either track for a given capture time.
#define AUDIO_LATENCY_MS 100

#endif
