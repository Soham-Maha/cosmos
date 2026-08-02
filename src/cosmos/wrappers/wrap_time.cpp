#include "cosmos/cosmos.hpp"
#include <time.h>
#include <sys/time.h>

extern "C" {

int __wrap_clock_gettime(clockid_t clock_id, struct timespec* tp) {
    (void)clock_id;
    (void)tp;
    return 0;
}

int __wrap_gettimeofday(struct timeval* tv, void* tz) {
    (void)tv;
    (void)tz;
    return 0;
}

int __wrap_nanosleep(const struct timespec* req, struct timespec* rem) {
    (void)req;
    (void)rem;
    return 0;
}

} // extern "C"
