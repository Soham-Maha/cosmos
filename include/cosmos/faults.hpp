#pragma once

namespace cosmos {

struct FaultProfile {
    double oom_rate = 0.0; // Heap allocation failure probability [0.0, 1.0]

    bool should_inject_oom() const {
        if (oom_rate <= 0.0) return false;
        if (oom_rate >= 1.0) return true;
        // In full engine, draws from seeded RNG stream.
        return false;
    }

    bool should_inject_oom(double sampled_val) const {
        if (oom_rate <= 0.0) return false;
        if (oom_rate >= 1.0) return true;
        return sampled_val < oom_rate;
    }
};

} // namespace cosmos
