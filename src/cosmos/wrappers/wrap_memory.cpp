#include "cosmos/cosmos.hpp"
#include <cstddef>

extern "C" {

void* __wrap_malloc(size_t size) {
    (void)size;
    return nullptr;
}

void __wrap_free(void* ptr) {
    (void)ptr;
}

void* __wrap_calloc(size_t nmemb, size_t size) {
    (void)nmemb;
    (void)size;
    return nullptr;
}

void* __wrap_realloc(void* ptr, size_t size) {
    (void)ptr;
    (void)size;
    return nullptr;
}

} // extern "C"
