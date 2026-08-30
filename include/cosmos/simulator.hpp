#pragma once

#include "cosmos/faults.hpp"
#include "cosmos/memory.hpp"
#include <cassert>
#include <optional>
#include <utility>

namespace cosmos {

// Placeholder injector: the slot exists and stays empty until P1 delivers the real engine.
struct NoInjector {};

// The injector is a template parameter, not a fixed type, so this header does not depend on the
// fault engine and tests can plug in a stub. A forward declaration would not work: std::optional
// instantiates traits on its element, so the type must be complete where the member is declared.
template <typename Injector> class BasicSimulator {
  public:
    BasicSimulator() = default;
    ~BasicSimulator() {
        if (current_sim_ == this) {
            current_sim_ = nullptr;
        }
    }

    // Copying would leave two objects claiming the same thread_local slot, and destroying either
    // one would clear it while the other is still current.
    BasicSimulator(const BasicSimulator&) = delete;
    BasicSimulator& operator=(const BasicSimulator&) = delete;

    static BasicSimulator* current() { return current_sim_; }

    static bool has_current() { return current_sim_ != nullptr; }

    static void set_current(BasicSimulator* sim) { current_sim_ = sim; }

    TrackedHeap& heap() { return heap_; }
    const TrackedHeap& heap() const { return heap_; }

    FaultProfile& faults() { return faults_; }
    const FaultProfile& faults() const { return faults_; }

    void set_faults(const FaultProfile& f) { faults_ = f; }

    bool has_injector() const { return injector_.has_value(); }

    // Callers check has_injector() first: this runs inside __wrap_malloc, where throwing is not an
    // option.
    Injector& injector() {
        assert(injector_.has_value());
        return *injector_;
    }
    const Injector& injector() const {
        assert(injector_.has_value());
        return *injector_;
    }

    template <typename... Args> Injector& emplace_injector(Args&&... args) {
        return injector_.emplace(std::forward<Args>(args)...);
    }

    void clear_injector() { injector_.reset(); }

  private:
    inline static thread_local BasicSimulator* current_sim_{nullptr};
    TrackedHeap heap_{};
    FaultProfile faults_{};
    std::optional<Injector> injector_{};
};

using Simulator = BasicSimulator<NoInjector>;

} // namespace cosmos
