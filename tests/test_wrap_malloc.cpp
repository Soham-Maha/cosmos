#include "cosmos/cosmos.hpp"
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

void test_passthrough_malloc() {
    assert(!cosmos::Simulator::has_current());
    void *ptr = malloc(64);
    assert(ptr != nullptr);
    free(ptr);
    std::cout << "[PASS] test_passthrough_malloc" << std::endl;
}

void test_active_sim_malloc_and_alignment() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    std::size_t test_sizes[] = {0, 1, 3, 7, 15, 16, 17, 31, 32, 33, 64, 1024};
    std::vector<void *> ptrs;

    for (std::size_t size : test_sizes) {
        void *ptr = malloc(size);
        assert(ptr != nullptr);

        // Strict 16-byte alignment assertion (alignof(std::max_align_t))
        std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ptr);
        assert(addr % alignof(std::max_align_t) == 0);

        // Verify canary magic in header preceding payload
        auto *header = reinterpret_cast<cosmos::AllocationHeader *>(
            static_cast<char *>(ptr) - sizeof(cosmos::AllocationHeader));
        assert(header->magic == cosmos::COSMOS_CANARY_MAGIC);
        assert(header->requested_size == size);

        ptrs.push_back(ptr);
    }

    assert(sim.heap().stats().active_allocations == ptrs.size());
    assert(sim.heap().stats().total_allocation_count == ptrs.size());

    for (void *p : ptrs) {
        free(p);
    }

    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_active_sim_malloc_and_alignment" << std::endl;
}

void test_oom_fault_injection() {
    cosmos::Simulator sim;
    cosmos::FaultProfile fp;
    fp.oom_rate = 1.0; // 100% OOM injection
    sim.set_faults(fp);
    cosmos::Simulator::set_current(&sim);

    errno = 0;
    void *ptr = malloc(128);
    assert(ptr == nullptr);
    assert(errno == ENOMEM);
    assert(sim.heap().stats().oom_fault_count == 1);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_oom_fault_injection" << std::endl;
}

void test_zero_byte_allocation() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    void *ptr = malloc(0);
    assert(ptr != nullptr);
    std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ptr);
    assert(addr % alignof(std::max_align_t) == 0);

    free(ptr);
    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_zero_byte_allocation" << std::endl;
}

void test_reentrancy_and_many_allocations() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    // Stress test container insertion inside TrackedHeap with 10,000 allocations
    std::vector<void *> ptrs;
    ptrs.reserve(10000);

    for (int i = 0; i < 10000; ++i) {
        void *p = malloc(i % 128 + 1);
        assert(p != nullptr);
        ptrs.push_back(p);
    }

    assert(sim.heap().stats().active_allocations == 10000);

    for (void *p : ptrs) {
        free(p);
    }

    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_reentrancy_and_many_allocations" << std::endl;
}

int main() {
    test_passthrough_malloc();
    test_active_sim_malloc_and_alignment();
    test_oom_fault_injection();
    test_zero_byte_allocation();
    test_reentrancy_and_many_allocations();
    std::cout << "All malloc wrapper tests passed successfully!" << std::endl;
    return 0;
}
