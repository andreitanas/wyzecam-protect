#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stream_reply.h"

int main(void) {
    // Coalesced empty requests, split headers, and a request-shaped byte
    // sequence inside a stats body must all preserve TCP message framing.
    static const uint8_t wire[] = {
        0, 0, 0, 12, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0xff, 0, 0,
        0, 1, 0, 0, 0, 1, 0, 0,
        0, 2, 0, 3, 0x80, 0, 1,
        0, 4, 0, 4, 0, 1, 0, 0,
        0, 1, 0, 0,
        0, 1, 0, 1, 0, // a nonempty type 1 is not the observed IDR request
    };
    for (size_t cut = 0; cut <= sizeof(wire); cut++) {
        struct stream_reply r = {0};
        unsigned count = stream_reply_feed(&r, wire, cut);
        count += stream_reply_feed(&r, wire + cut, sizeof(wire) - cut);
        assert(count == 3 && !r.have && !r.remaining);
    }
    struct stream_reply r = {0};
    unsigned count = 0;
    for (size_t i = 0; i < sizeof(wire); i++)
        count += stream_reply_feed(&r, wire + i, 1);
    assert(count == 3 && !r.have && !r.remaining);

    // Large unknown messages need no body allocation and must not expose
    // their payload as new message headers.
    uint8_t *large = calloc(1, 4 + 65535 + 4);
    assert(large);
    large[1] = 99;
    large[2] = large[3] = 255;
    large[5] = large[4 + 65535 + 1] = 1;
    r = (struct stream_reply){0};
    assert(stream_reply_feed(&r, large, 4 + 65535 + 4) == 1);
    assert(!r.have && !r.remaining);
    free(large);

    // A latency probe, as Protect sent one to a G5, whole and byte by byte
    static const uint8_t probe[] = {0, 4, 0, 16, 'a', 'n', 'd', 'd', 0, 0, 3, 0xe8,
                                    0, 0, 0, 0, 0, 0, 0x55, 0x4e, 0, 1, 0, 0};
    r = (struct stream_reply){0};
    assert(stream_reply_feed(&r, probe, sizeof(probe)) == 1 && r.nprobes == 1);
    assert(!strcmp(r.probes[0].id, "andd") && r.probes[0].rate == 1000 && r.probes[0].time == 21838);
    r = (struct stream_reply){0};
    unsigned probes = 0;
    for (size_t i = 0; i < sizeof(probe); i++) {
        stream_reply_feed(&r, probe + i, 1);
        if (r.nprobes)
            assert(i == 19 && r.probes[0].time == 21838), probes++;
    }
    assert(probes == 1);
    puts("Protect feedback framing: passed");
    return 0;
}
