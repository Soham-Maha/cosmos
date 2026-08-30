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

/**
 * @brief Linker interposition wrapper for standard C `malloc(size)`.
 *
 * WHAT IT IS:
 * Intercepts all calls to standard C `malloc` at final link time when compiled with
 * `-Wl,--wrap=malloc`.
 *
 * WHY & WHEN IT IS USED:
 * - Used during testing builds (`-DCOSMOS_SIM`) to capture memory allocation calls in application
 * code and linked libraries.
 * - Instantiates `ReentrancyGuard` immediately after checking `in_wrap_malloc` to ensure all
 * subsequent operations
 *   (`Simulator::has_current()`, `faults().should_inject_oom()`, `heap().record_oom()`, and
 * `heap().allocate(size)`) are fully protected from recursive re-entrancy loops if any internal
 * operation calls `malloc`.
 * - When an active `Simulator` context (`Simulator::has_current()`) is running:
 *   1. Checks deterministic OOM fault injection (`sim->faults().should_inject_oom()`). If
 * triggered, sets `errno = ENOMEM` and returns `nullptr`.
 *   2. Delegates to `sim->heap().allocate(size)` to attach allocation metadata headers and record
 * stats.
 * - Outside an active simulation context or during internal re-entrant allocations (`in_wrap_malloc
 * == true`), falls back directly to native OS `__real_malloc(size)`.
 */
void* __wrap_malloc(size_t size) {
    if (in_wrap_malloc) {
        return __real_malloc(size);
    }

    ReentrancyGuard guard;

    if (!cosmos::Simulator::has_current()) {
        return __real_malloc(size);
    }

    auto* sim = cosmos::Simulator::current();
    if (sim->faults().should_inject_oom()) {
        errno = ENOMEM;
        sim->heap().record_oom();
        return nullptr;
    }

    return sim->heap().allocate(size);
}

/**
 * @brief Linker interposition wrapper for standard C `free(ptr)`.
 *
 * WHAT IT IS:
 * Intercepts all calls to standard C `free` at final link time when compiled with
 * `-Wl,--wrap=free`.
 *
 * WHY & WHEN IT IS USED:
 * - Used during testing builds (`-DCOSMOS_SIM`) to capture heap deallocation calls.
 * - Immediately returns if `ptr == nullptr`.
 * - Instantiates `ReentrancyGuard` to protect context lookups and `TrackedHeap::deallocate` from
 * recursive re-entrancy loops.
 * - If inside an active `Simulator` context (`Simulator::has_current()`):
 *   Delegates to `sim->heap().deallocate(ptr)`. If `deallocate` recognizes the pointer's header
 * canary magic, it updates active heap statistics, marks the header as freed, and frees the raw
 * header via `__real_free`.
 * - For passthrough allocations (allocated via `__real_malloc` outside a simulation context) or
 * during re-entrancy, falls back directly to native OS `__real_free(ptr)`.
 */
void __wrap_free(void* ptr) {
    if (!ptr) return;

    if (in_wrap_malloc) {
        __real_free(ptr);
        return;
    }

    ReentrancyGuard guard;

    if (cosmos::Simulator::has_current()) {
        auto* sim = cosmos::Simulator::current();
        if (sim->heap().deallocate(ptr)) {
            return;
        }
    }

    // The owning Simulator may no longer be current, so the canary is checked here too: handing a
    // tracked payload to __real_free would free an address one header past the real allocation.
    auto* header = cosmos::header_for(ptr);
    if (header->magic == cosmos::COSMOS_CANARY_MAGIC) {
        header->magic = cosmos::COSMOS_FREED_MAGIC;
        __real_free(header);
        return;
    }

    // Passthrough allocation (allocated via __real_malloc outside simulation context)
    __real_free(ptr);
}

void* __wrap_calloc(size_t nmemb, size_t size) { return __real_calloc(nmemb, size); }

void* __wrap_realloc(void* ptr, size_t size) { return __real_realloc(ptr, size); }

} // extern "C"
