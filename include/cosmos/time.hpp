#pragma once

#include <compare>
#include <cstdint>

namespace cosmos {

// Signed overflow is UB, and a wrapped timestamp would silently reorder events in a run that is
// supposed to be reproducible. These saturate instead, so arithmetic is total and gives the same
// answer in debug and release. Same names and semantics as C++26 std::add_sat, which replaces
// them once the project moves off C++23.
constexpr int64_t add_sat(int64_t a, int64_t b) {
    int64_t result = 0;
    if (!__builtin_add_overflow(a, b, &result)) return result;
    return b > 0 ? INT64_MAX : INT64_MIN;
}

constexpr int64_t sub_sat(int64_t a, int64_t b) {
    int64_t result = 0;
    if (!__builtin_sub_overflow(a, b, &result)) return result;
    return b > 0 ? INT64_MIN : INT64_MAX;
}

constexpr int64_t mul_sat(int64_t a, int64_t b) {
    int64_t result = 0;
    if (!__builtin_mul_overflow(a, b, &result)) return result;
    return (a > 0) == (b > 0) ? INT64_MAX : INT64_MIN;
}

// Time and Duration are both a 64-bit nanosecond count, but they follow different rules: adding
// two timestamps is meaningless, adding two durations is not. The tag parameter keeps them
// separate types at compile time and costs nothing at runtime.
template <typename Tag> struct Nanos {
    int64_t ns{0};

    static constexpr Nanos zero() { return Nanos{0}; }
    static constexpr Nanos max() { return Nanos{INT64_MAX}; }
    static constexpr Nanos min() { return Nanos{INT64_MIN}; }

    constexpr bool operator==(const Nanos&) const = default;
    constexpr auto operator<=>(const Nanos&) const = default;
};

struct TimeTag {};
struct DurationTag {};

// Virtual time since universe start. Time::max() reads as "never" (the default quiesce_after),
// and saturating arithmetic keeps it that way.
using Time = Nanos<TimeTag>;
using Duration = Nanos<DurationTag>;

constexpr Duration operator+(Duration a, Duration b) { return Duration{add_sat(a.ns, b.ns)}; }
constexpr Duration operator-(Duration a, Duration b) { return Duration{sub_sat(a.ns, b.ns)}; }
constexpr Duration operator-(Duration d) { return Duration{sub_sat(0, d.ns)}; }
constexpr Duration operator*(Duration d, int64_t k) { return Duration{mul_sat(d.ns, k)}; }
constexpr Duration operator*(int64_t k, Duration d) { return Duration{mul_sat(d.ns, k)}; }

constexpr Time operator+(Time t, Duration d) { return Time{add_sat(t.ns, d.ns)}; }
constexpr Time operator+(Duration d, Time t) { return Time{add_sat(t.ns, d.ns)}; }
constexpr Time operator-(Time t, Duration d) { return Time{sub_sat(t.ns, d.ns)}; }
constexpr Duration operator-(Time a, Time b) { return Duration{sub_sat(a.ns, b.ns)}; }

constexpr Duration& operator+=(Duration& a, Duration b) { return a = a + b; }
constexpr Duration& operator-=(Duration& a, Duration b) { return a = a - b; }
constexpr Duration& operator*=(Duration& d, int64_t k) { return d = d * k; }
constexpr Time& operator+=(Time& t, Duration d) { return t = t + d; }
constexpr Time& operator-=(Time& t, Duration d) { return t = t - d; }

namespace literals {

constexpr int64_t to_ns(unsigned long long value, int64_t unit) {
    constexpr auto limit = static_cast<unsigned long long>(INT64_MAX);
    return mul_sat(value > limit ? INT64_MAX : static_cast<int64_t>(value), unit);
}

constexpr Duration operator""_ns(unsigned long long v) { return Duration{to_ns(v, 1)}; }
constexpr Duration operator""_us(unsigned long long v) { return Duration{to_ns(v, 1'000)}; }
constexpr Duration operator""_ms(unsigned long long v) { return Duration{to_ns(v, 1'000'000)}; }
constexpr Duration operator""_s(unsigned long long v) { return Duration{to_ns(v, 1'000'000'000)}; }

} // namespace literals

} // namespace cosmos
