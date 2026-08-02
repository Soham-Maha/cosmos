#include "cosmos/cosmos.hpp"
#include <sys/random.h>
#include <stdlib.h>

extern "C" {

ssize_t __wrap_getrandom(void* buf, size_t buflen, unsigned int flags) {
    (void)buf;
    (void)buflen;
    (void)flags;
    return (ssize_t)buflen;
}

long int __wrap_random(void) {
    return 0;
}

} // extern "C"
