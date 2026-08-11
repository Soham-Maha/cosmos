#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <unordered_map>

extern "C" {
void *__real_malloc(size_t size);
void __real_free(void *ptr);
}

namespace cosmos {

constexpr uint32_t COSMOS_CANARY_MAGIC = 0x434F534D; // 'COSM'
constexpr uint32_t COSMOS_FREED_MAGIC = 0xDEADBEEF;

struct alignas(std::max_align_t) AllocationHeader {
    uint32_t magic;        // Canary magic value
    uint32_t flags;        // Header state / flags
    size_t requested_size; // Size requested by caller
    uint64_t alloc_id;     // Sequential allocation ID
    uint64_t padding;      // Padding to ensure size is a multiple of max_align_t
};

static_assert(sizeof(AllocationHeader) % alignof(std::max_align_t) == 0,
              "AllocationHeader size must be a multiple of max_align_t");

template <typename T> struct RawRealAllocator {
    using value_type = T;

    RawRealAllocator() noexcept = default;
    template <typename U> constexpr RawRealAllocator(const RawRealAllocator<U> &) noexcept {}

    T *allocate(std::size_t n) {
        if (n > std::size_t(-1) / sizeof(T)) {
            throw std::bad_alloc();
        }
        void *ptr = __real_malloc(n * sizeof(T));
        if (!ptr) {
            throw std::bad_alloc();
        }
        return static_cast<T *>(ptr);
    }

    void deallocate(T *p, std::size_t) noexcept { __real_free(static_cast<void *>(p)); }
};

template <typename T, typename U>
bool operator==(const RawRealAllocator<T> &, const RawRealAllocator<U> &) {
    return true;
}
template <typename T, typename U>
bool operator!=(const RawRealAllocator<T> &, const RawRealAllocator<U> &) {
    return false;
}

struct HeapStats {
    size_t total_allocated_bytes = 0;
    size_t active_allocations = 0;
    size_t total_allocation_count = 0;
    size_t oom_fault_count = 0;
};

class TrackedHeap {
  public:
    TrackedHeap() = default;

    void *allocate(size_t size) {
        constexpr size_t header_size = sizeof(AllocationHeader);
        size_t total_size = header_size + size;
        void *raw = __real_malloc(total_size);
        if (!raw) {
            return nullptr;
        }

        uint64_t id = ++next_alloc_id_;
        auto *header = static_cast<AllocationHeader *>(raw);
        header->magic = COSMOS_CANARY_MAGIC;
        header->flags = 0;
        header->requested_size = size;
        header->alloc_id = id;
        header->padding = 0;

        stats_.total_allocated_bytes += size;
        stats_.active_allocations++;
        stats_.total_allocation_count++;

        void *user_ptr = static_cast<char *>(raw) + header_size;
        active_map_[user_ptr] = *header;
        return user_ptr;
    }

    bool deallocate(void *user_ptr) {
        if (!user_ptr)
            return false;

        constexpr size_t header_size = sizeof(AllocationHeader);
        auto *header =
            reinterpret_cast<AllocationHeader *>(static_cast<char *>(user_ptr) - header_size);

        if (header->magic != COSMOS_CANARY_MAGIC) {
            return false;
        }

        header->magic = COSMOS_FREED_MAGIC;
        if (stats_.active_allocations > 0) {
            stats_.active_allocations--;
        }
        active_map_.erase(user_ptr);

        void *raw_ptr = header;
        __real_free(raw_ptr);
        return true;
    }

    void record_oom() { stats_.oom_fault_count++; }

    const HeapStats &stats() const { return stats_; }
    size_t active_count() const { return stats_.active_allocations; }

  private:
    uint64_t next_alloc_id_{0};
    HeapStats stats_{};
    using MapType =
        std::unordered_map<void *, AllocationHeader, std::hash<void *>, std::equal_to<void *>,
                           RawRealAllocator<std::pair<void *const, AllocationHeader>>>;
    MapType active_map_{};
};

} // namespace cosmos
