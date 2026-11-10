#ifndef UVC_STREAM_REPLY_H
#define UVC_STREAM_REPLY_H

#include <stddef.h>
#include <stdint.h>

// Protect's TCP feedback: big-endian u16 type, u16 body length, then body.
// TCP can split or join messages. Seen (a G5's connection, 2026-11-09):
//   type 1, empty: an IDR request;
//   type 4, 16 bytes: a client's latency probe, forwarded: its 4-character
//     id ("andd", "iosd": Protect's Android and iOS apps), a clock rate
//     (1000) and the time of a video frame it received, on the camera's
//     clock (onClockSync's streamClockBase + the tag's timestamp). The
//     camera answers with an onRtt script tag (flv.h);
//   type 0, 12 bytes: receiver statistics once a second; type 2, 3 bytes
//     at the start; type 5, 2 bytes. Not used.
struct stream_probe {
    char id[5];
    uint32_t rate;
    uint64_t time;
};

struct stream_reply {
    uint8_t header[4];
    unsigned have, remaining;
    unsigned type, got; // the current message, and how much of its body is in body
    uint8_t body[16];
    struct stream_probe probes[4]; // from the last feed
    unsigned nprobes;
};

// The number of IDR requests in data; the probes in it are in r->probes
// (the first four).
unsigned stream_reply_feed(struct stream_reply *r, const uint8_t *data, size_t len);

#endif
