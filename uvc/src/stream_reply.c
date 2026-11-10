#include "stream_reply.h"

#include <string.h>

static void message(struct stream_reply *r) {
    if (r->type != 4 || r->got != 16 || r->nprobes == sizeof(r->probes) / sizeof(r->probes[0]))
        return;
    struct stream_probe *p = &r->probes[r->nprobes++];
    const uint8_t *b = r->body;
    memcpy(p->id, b, 4);
    p->id[4] = 0;
    p->rate = (uint32_t)b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7];
    p->time = 0;
    for (int i = 8; i < 16; i++)
        p->time = p->time << 8 | b[i];
}

unsigned stream_reply_feed(struct stream_reply *r, const uint8_t *data, size_t len) {
    unsigned requests = 0;
    r->nprobes = 0;
    while (len) {
        if (r->remaining) {
            size_t n = len < r->remaining ? len : r->remaining;
            // Only short bodies are kept: nothing longer is understood
            for (size_t i = 0; i < n && r->got < sizeof(r->body); i++)
                r->body[r->got++] = data[i];
            r->remaining -= n;
            data += n;
            len -= n;
            if (!r->remaining)
                message(r);
            continue;
        }
        size_t n = len < 4 - r->have ? len : 4 - r->have;
        memcpy(r->header + r->have, data, n);
        r->have += n;
        data += n;
        len -= n;
        if (r->have == 4) {
            r->type = (unsigned)r->header[0] << 8 | r->header[1];
            r->remaining = (unsigned)r->header[2] << 8 | r->header[3];
            r->got = 0;
            if (r->type == 1 && !r->remaining)
                requests++;
            r->have = 0;
        }
    }
    return requests;
}
