#include "cosmos/cosmos.hpp"
#include <cstddef>

// Linker-wrapping passthrough stubs. Under the sim build (-Wl,--wrap=malloc ...)
// every reference to malloc/free/calloc/realloc resolves to these __wrap_ symbols.
// The matching __real_* aliases (pointing at the real libc functions) are provided
// by the linker's --wrap mechanism at final-link time of the sim executable
// (see docs/architecture.md §3). The deterministic tracked heap / OOM fault
// injection is layered on later; for now they pass through unchanged.

extern "C" {

void* __real_malloc(size_t size);
void  __real_free(void* ptr);
void* __real_calloc(size_t nmemb, size_t size);
void* __real_realloc(void* ptr, size_t size);

void* __wrap_malloc(size_t size) {
    return __real_malloc(size);
}

void __wrap_free(void* ptr) {
    __real_free(ptr);
}

void* __wrap_calloc(size_t nmemb, size_t size) {
    return __real_calloc(nmemb, size);
}

void* __wrap_realloc(void* ptr, size_t size) {
    return __real_realloc(ptr, size);
}

} // extern "C"
