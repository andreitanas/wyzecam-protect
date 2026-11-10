// The camera's malloc: Doug Lea's (third_party/dlmalloc), in place of the one
// in zig's libc.
//
// zig 0.17's libc gives musl targets Zig's SmpAllocator, which keeps one lock
// per CPU and, when a thread's lock is taken, tries the next CPU's in a loop
// without sleeping. The T20 has one CPU, so one lock. When the detector
// thread (nice 19) was preempted holding it, every other thread that called
// malloc spun at normal priority, which kept the detector from running and
// releasing it: both capture threads stood in malloc for 0.3 to 5 s, a few
// times an hour to once a minute (docs/kernel-modern.md, Memory allocator).
// This one's lock is a pthread mutex: waiters sleep and the holder runs.
//
// zig's libc exports its allocator's functions weakly, so these definitions
// replace them, for libc's own callers too.
#if defined(__linux__) && defined(__mips__)

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#define USE_LOCKS 1
#define USE_SPIN_LOCKS 0 // pthread mutexes
#define HAVE_MORECORE 0  // mmap only: no sbrk
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define LACKS_SCHED_H 1

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wnull-pointer-arithmetic"
#pragma GCC diagnostic ignored "-Wexpansion-to-defined"
#include "../third_party/dlmalloc/malloc.c"
#pragma GCC diagnostic pop

// The rest of what zig's libc defines on its allocator
void *aligned_alloc(size_t alignment, size_t size) { return memalign(alignment, size); }

void *reallocarray(void *p, size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(p, n * size);
}

#endif
