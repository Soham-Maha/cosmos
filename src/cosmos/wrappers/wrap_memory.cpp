#include "cosmos/cosmos.hpp"
#include <cerrno>
#include <cstddef>

namespace {
thread_local bool in_wrap_malloc = false;

struct ReentrancyGuard {
    ReentrancyGuard() { in_wrap_malloc = true; }
    ~ReentrancyGuard() { in_wrap_malloc = false; }
};
} // namespace

extern "C" {

void* __real_malloc(size_t size);
void __real_free(void* ptr);
void* __real_calloc(size_t nmemb, size_t size);
void* __real_realloc(void* ptr, size_t size);

void* __wrap_malloc(size_t size) {
    if (in_wrap_malloc) {
        return __real_malloc(size);
    }

    if (!cosmos::Simulator::has_current()) {
        return __real_malloc(size);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->faults().should_inject_oom()) {
        errno = ENOMEM;
        sim->heap().record_oom();
        return nullptr;
    }

    ReentrancyGuard guard;
    return sim->heap().allocate(size);
}

void __wrap_free(void* ptr) {
    if (!ptr) return;

    if (in_wrap_malloc) {
        __real_free(ptr);
        return;
    }

    if (cosmos::Simulator::has_current()) {
        auto* sim = cosmos::Simulator::current();
        ReentrancyGuard guard;
        if (sim->heap().deallocate(ptr)) {
            return;
        }
    }

    // Passthrough allocation (allocated via __real_malloc outside simulation context)
    __real_free(ptr);
}

void* __wrap_calloc(size_t nmemb, size_t size) { return __real_calloc(nmemb, size); }

void* __wrap_realloc(void* ptr, size_t size) { return __real_realloc(ptr, size); }

} // extern "C"
