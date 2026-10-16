#pragma once

#include "cosmos/faults.hpp"
#include "cosmos/time.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cosmos {

// One entry per post-gate decision, in the order the decisions happened (§11.2). Gate-blocked
// calls never reach the boundary and are counted instead, so the ledger stays fires-only.
struct TraceEntry {
    uint64_t seq;
    uint64_t eligible_index;
    Time at;
    SiteId site;
    Duration amount;
    FaultKind outcome;
    bool drew;
    DecisionStatus status = DecisionStatus::Fired;

    constexpr bool operator==(const TraceEntry&) const = default;
};

// 1M entries is ~48 MB, allocated once per universe, and only when a trace was asked for.
inline constexpr uint64_t kDefaultTraceCapacity = 1 << 20;

struct TraceOptions {
    bool enabled = false;
    uint64_t capacity = kDefaultTraceCapacity;
};

// Fixed capacity, never grows: decide() runs inside __wrap_malloc, so recording must not allocate
// (Rule 7). Overflow is counted rather than dropped silently, like FaultLedger's.
class FaultTrace {
  public:
    FaultTrace() = default;

    explicit FaultTrace(uint64_t capacity) : capacity_(capacity) {
        if (capacity_ > 0) entries_.reserve(static_cast<size_t>(capacity_));
    }

    bool enabled() const { return capacity_ > 0; }
    uint64_t capacity() const { return capacity_; }
    uint64_t dropped() const { return dropped_; }

    void record(const TraceEntry& entry) {
        if (capacity_ == 0) return;
        if (entries_.size() == capacity_) {
            ++dropped_;
            return;
        }
        entries_.push_back(entry);
    }

    size_t size() const { return entries_.size(); }
    const TraceEntry& operator[](size_t index) const { return entries_[index]; }
    const TraceEntry* begin() const { return entries_.data(); }
    const TraceEntry* end() const { return entries_.data() + entries_.size(); }

  private:
    std::vector<TraceEntry> entries_{};
    uint64_t capacity_ = 0;
    uint64_t dropped_ = 0;
};

} // namespace cosmos
