/*
 * PS1 port — minimal <time.h>.
 *
 * Only used by Math.cpp's srand(time(NULL)). The port has no RTC clock yet,
 * so time() returns 0 (deterministic seed — fine for a boot stub). A real
 * clock (psyqo Kernel ticks or the PS1 RTC) is a Phase 6 item.
 */
#ifndef PS1_TIME_H
#define PS1_TIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long time_t;
typedef long clock_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

static inline time_t time(time_t *tloc) {
    if (tloc)
        *tloc = 0;
    return 0;
}

static inline clock_t clock(void) {
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* PS1_TIME_H */
