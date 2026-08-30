#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <unordered_map>

extern "C" {
void* __real_malloc(size_t size);
void __real_free(void* ptr);
}

namespace cosmos {

constexpr uint32_t COSMOS_CANARY_MAGIC = 0x434F534D; // 'COSM'
constexpr uint32_t COSMOS_FREED_MAGIC = 0xDEADBEEF;

/**
 * @brief AllocationHeader prepended to every memory allocation in TrackedHeap.
 *
 * Aligned to alignof(std::max_align_t) (16 bytes) and padded so that sizeof(AllocationHeader)
 * is a multiple of 16 bytes. This guarantees that the user payload pointer returned by
 * (char*)header + sizeof(AllocationHeader) preserves strict 16-byte alignment.
 */
struct alignas(std::max_align_t) AllocationHeader {
    uint32_t magic;        // Canary magic value (COSMOS_CANARY_MAGIC or COSMOS_FREED_MAGIC)
    uint32_t flags;        // Metadata flags
    size_t requested_size; // User payload size requested by caller
    uint64_t alloc_id;     // Sequential allocation ID
    uint64_t padding;      // Padding ensuring sizeof(AllocationHeader) == 32 (multiple of 16)
};

static_assert(sizeof(AllocationHeader) % alignof(std::max_align_t) == 0,
              "AllocationHeader size must be a multiple of max_align_t");

// Every tracked payload is preceded by its header, so a tracked pointer must never be handed to
// __real_free directly: the real allocation starts sizeof(AllocationHeader) bytes earlier.
inline AllocationHeader* header_for(void* user_ptr) {
    return reinterpret_cast<AllocationHeader*>(static_cast<char*>(user_ptr) -
                                               sizeof(AllocationHeader));
}

/**
 * @brief Custom C++ allocator for TrackedHeap internal containers (e.g. std::unordered_map).
 *
 * WHAT IT IS:
 * An allocator implementation that routes memory allocations directly to `__real_malloc` and
 * `__real_free`.
 *
 * WHY & WHEN IT IS USED:
 * Under `-Wl,--wrap=malloc`, standard container allocations (using default std::allocator) call
 * `malloc()`, which resolves back to `__wrap_malloc()`. If `TrackedHeap`'s internal hash map used
 * default allocators, allocating map nodes inside `TrackedHeap::allocate` would trigger an infinite
 * recursive stack overflow. `RawRealAllocator` explicitly bypasses symbol wrapping for internal
 * metadata storage.
 */
template <typename T> struct RawRealAllocator {
    using value_type = T;

    RawRealAllocator() noexcept = default;
    template <typename U> constexpr RawRealAllocator(const RawRealAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n > std::size_t(-1) / sizeof(T)) {
            throw std::bad_alloc();
        }
        void* ptr = __real_malloc(n * sizeof(T));
        if (!ptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(ptr);
    }

    void deallocate(T* p, std::size_t) noexcept { __real_free(static_cast<void*>(p)); }
};

template <typename T, typename U>
bool operator==(const RawRealAllocator<T>&, const RawRealAllocator<U>&) {
    return true;
}
template <typename T, typename U>
bool operator!=(const RawRealAllocator<T>&, const RawRealAllocator<U>&) {
    return false;
}

/**
 * @brief Cumulative memory allocation statistics for a simulation universe.
 */
struct HeapStats {
    size_t total_allocated_bytes = 0;
    size_t active_allocations = 0;
    size_t total_allocation_count = 0;
    size_t oom_fault_count = 0;
};

/**
 * @brief Internal simulation engine memory tracker.
 *
 * WHAT IT IS:
 * The deterministic heap tracking manager owned by `Simulator`. It maintains memory allocation
 * headers, active allocation maps, and cumulative heap statistics (`HeapStats`).
 *
 * WHY & WHEN IT IS USED:
 * Used by `__wrap_malloc` and `__wrap_free` whenever an active `Simulator` context
 * (`Simulator::has_current()`) is running:
 * - On `allocate(size)`: Allocates `sizeof(AllocationHeader) + size` via `__real_malloc`,
 * initializes the header with canary magic and metadata, records the allocation in `active_map_`,
 * and returns the 16-byte aligned user pointer.
 * - On `deallocate(user_ptr)`: Inspects the header, verifies canary magic, marks the header as
 * freed (`COSMOS_FREED_MAGIC`), erases from `active_map_`, updates active allocation counts, and
 * frees the raw header via `__real_free`.
 * - Enables post-simulation memory leak detection (verifying `active_count() == 0`) and canary
 * corruption validation.
 */
class TrackedHeap {
  public:
    TrackedHeap() = default;

    void* allocate(size_t size) {
        constexpr size_t header_size = sizeof(AllocationHeader);
        size_t total_size = header_size + size;
        void* raw = __real_malloc(total_size);
        if (!raw) {
            return nullptr;
        }

        uint64_t id = ++next_alloc_id_;
        auto* header = static_cast<AllocationHeader*>(raw);
        header->magic = COSMOS_CANARY_MAGIC;
        header->flags = 0;
        header->requested_size = size;
        header->alloc_id = id;
        header->padding = 0;

        stats_.total_allocated_bytes += size;
        stats_.active_allocations++;
        stats_.total_allocation_count++;

        void* user_ptr = static_cast<char*>(raw) + header_size;
        active_map_[user_ptr] = *header;
        return user_ptr;
    }

    bool deallocate(void* user_ptr) {
        if (!user_ptr) return false;

        auto* header = header_for(user_ptr);
        if (header->magic != COSMOS_CANARY_MAGIC) {
            return false;
        }

        header->magic = COSMOS_FREED_MAGIC;
        if (stats_.active_allocations > 0) {
            stats_.active_allocations--;
        }
        active_map_.erase(user_ptr);

        void* raw_ptr = header;
        __real_free(raw_ptr);
        return true;
    }

    void record_oom() { stats_.oom_fault_count++; }

    const HeapStats& stats() const { return stats_; }
    size_t active_count() const { return stats_.active_allocations; }

  private:
    uint64_t next_alloc_id_{0};
    HeapStats stats_{};
    using MapType =
        std::unordered_map<void*, AllocationHeader, std::hash<void*>, std::equal_to<void*>,
                           RawRealAllocator<std::pair<void* const, AllocationHeader>>>;
    MapType active_map_{};
};

} // namespace cosmos
