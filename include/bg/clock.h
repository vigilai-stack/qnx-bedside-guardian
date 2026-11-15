#ifndef BG_CLOCK_H
#define BG_CLOCK_H

#include <stdint.h>
#include <time.h>

static inline uint64_t bg_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif
