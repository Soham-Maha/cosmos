#pragma once

#include "cosmos/faults.hpp"
#include "cosmos/memory.hpp"

namespace cosmos {

class Simulator {
  public:
    Simulator() = default;
    ~Simulator() {
        if (current_sim_ == this) {
            current_sim_ = nullptr;
        }
    }

    static Simulator *current() { return current_sim_; }

    static bool has_current() { return current_sim_ != nullptr; }

    static void set_current(Simulator *sim) { current_sim_ = sim; }

    TrackedHeap &heap() { return heap_; }
    const TrackedHeap &heap() const { return heap_; }

    FaultProfile &faults() { return faults_; }
    const FaultProfile &faults() const { return faults_; }

    void set_faults(const FaultProfile &f) { faults_ = f; }

  private:
    inline static thread_local Simulator *current_sim_{nullptr};
    TrackedHeap heap_{};
    FaultProfile faults_{};
};

} // namespace cosmos
