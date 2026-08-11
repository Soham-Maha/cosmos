#include "cosmos/cosmos.hpp"
#include <stdlib.h>
#include <sys/random.h>

// Linker-wrapping passthrough stubs for the POSIX randomness surface
// (see docs/design.md §3 "Random"). The seeded xoshiro256** / domain-isolated
// PRNG streams are layered on later; for now they pass through to the real
// host entropy via __real_*.

extern "C" {

ssize_t __real_getrandom(void *buf, size_t buflen, unsigned int flags);
long int __real_random(void);

ssize_t __wrap_getrandom(void *buf, size_t buflen, unsigned int flags) {
    return __real_getrandom(buf, buflen, flags);
}

long int __wrap_random(void) { return __real_random(); }

} // extern "C"
