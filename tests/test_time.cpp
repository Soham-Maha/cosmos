#include "cosmos/time.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <type_traits>

using namespace cosmos::literals;
using cosmos::Duration;
using cosmos::Time;

// Both types must stay a bare 64-bit count: the P5 canonical trace encoding writes them as int64
// nanoseconds, and the engine copies them freely.
static_assert(sizeof(Time) == 8 && sizeof(Duration) == 8);
static_assert(std::is_trivially_copyable_v<Time> && std::is_trivially_copyable_v<Duration>);

// The whole surface is usable in constant expressions, so config windows can be compile-time.
static_assert(1_s + 500_ms == 1500_ms);
static_assert(Time::zero() + 2_s > Time::zero());

// The operands must be template parameters: a requires-expression over concrete types is a hard
// compile error rather than a false result.
template <typename A, typename B>
concept can_add = requires(A a, B b) { a + b; };
template <typename A, typename B>
concept can_subtract = requires(A a, B b) { a - b; };
template <typename A, typename B>
concept can_compare = requires(A a, B b) { a == b; };
template <typename A>
concept can_scale = requires(A a) { a * 2; };

// The tag parameter is what rejects meaningless arithmetic; these must never start compiling.
static_assert(!can_add<Time, Time>);
static_assert(!can_scale<Time>);
static_assert(!can_compare<Time, Duration>);
static_assert(!std::is_convertible_v<Time, Duration>);
static_assert(!std::is_convertible_v<Duration, Time>);

// The meaningful combinations stay available, so the checks above are not vacuously true.
static_assert(can_add<Time, Duration>);
static_assert(can_add<Duration, Duration>);
static_assert(can_subtract<Time, Time>);
static_assert(can_subtract<Time, Duration>);
static_assert(can_scale<Duration>);
static_assert(can_compare<Time, Time>);
static_assert(can_compare<Duration, Duration>);

void test_duration_arithmetic() {
    assert(1_s + 500_ms == 1500_ms);
    assert(1500_ms - 500_ms == 1_s);
    assert(2_s * 3 == 6_s);
    assert(3 * 2_s == 6_s);
    assert(-(1_s) == Duration{-1'000'000'000});

    assert(1_s == 1000_ms);
    assert(1_ms == 1000_us);
    assert(1_us == 1000_ns);
    assert(1_s == 1'000'000'000_ns);

    Duration d = 1_s;
    d += 500_ms;
    assert(d == 1500_ms);
    d -= 1_s;
    assert(d == 500_ms);
    d *= 4;
    assert(d == 2_s);

    std::cout << "[PASS] test_duration_arithmetic" << std::endl;
}

void test_ordering_and_bounds() {
    assert(1_ms < 1_s);
    assert(1_s > 1_ms);
    assert(1_s <= 1_s);
    assert(1_s >= 1_s);
    assert(1_s != 2_s);

    assert(Duration::zero().ns == 0);
    assert(Duration::max().ns == INT64_MAX);
    assert(Duration::min().ns == INT64_MIN);
    assert(Duration::min() < Duration::zero());
    assert(Duration::zero() < Duration::max());

    assert(Time::zero().ns == 0);
    assert(Time::zero() < Time::max());
    assert(Time::min() < Time::zero());

    std::cout << "[PASS] test_ordering_and_bounds" << std::endl;
}

void test_time_and_duration_mix() {
    Time start = Time::zero();
    Time later = start + 5_s;
    assert(later.ns == 5'000'000'000);
    assert(5_s + start == later);
    assert(later - 5_s == start);
    assert(later - start == 5_s);
    assert((start + 10_s) - (start + 3_s) == 7_s);

    Time cursor = start;
    cursor += 250_ms;
    assert(cursor == start + 250_ms);
    cursor -= 250_ms;
    assert(cursor == start);

    std::cout << "[PASS] test_time_and_duration_mix" << std::endl;
}

// Overflow saturates rather than asserting. Saturating is total, has no undefined behavior, and
// returns the same value under NDEBUG as under -UNDEBUG; an assert would vanish in release builds
// and leave signed-overflow UB in a path the whole engine's determinism depends on.
void test_overflow_saturates() {
    assert(Duration::max() + 1_ns == Duration::max());
    assert(Duration::max() + Duration::max() == Duration::max());
    assert(Duration::min() - 1_ns == Duration::min());
    assert(Duration::min() + Duration::min() == Duration::min());

    assert(Duration::max() * 2 == Duration::max());
    assert(Duration::max() * -2 == Duration::min());
    assert(Duration::min() * 2 == Duration::min());
    assert(Duration::min() * -1 == Duration::max());
    assert(-Duration::min() == Duration::max());

    assert(Duration::max() * 0 == Duration::zero());
    assert(Duration::max() * 1 == Duration::max());

    // "Never" must stay "never": Time::max() is the default quiesce_after.
    assert(Time::max() + 1_s == Time::max());
    assert(Time::min() - 1_s == Time::min());
    assert(Time::max() - Time::min() == Duration::max());

    // A literal too large for int64 nanoseconds clamps instead of wrapping.
    assert(10'000'000'000_s == Duration::max());
    assert(18446744073709551615_ns == Duration::max());

    std::cout << "[PASS] test_overflow_saturates" << std::endl;
}

// P1's validate() rejects warmup_until > quiesce_after; this proves the comparison is expressible
// on the types themselves, with Time::max() as the "no quiesce configured" default.
void test_run_windows_are_orderable() {
    Time warmup_until = Time::zero() + 10_ms;
    Time quiesce_after = Time::zero() + 900_ms;
    assert(warmup_until <= quiesce_after);

    Time never = Time::max();
    assert(warmup_until <= never);
    assert(quiesce_after <= never);

    Time inverted_warmup = Time::zero() + 2_s;
    assert(!(inverted_warmup <= quiesce_after));

    assert(Time::zero() <= Time::zero());

    std::cout << "[PASS] test_run_windows_are_orderable" << std::endl;
}

int main() {
    test_duration_arithmetic();
    test_ordering_and_bounds();
    test_time_and_duration_mix();
    test_overflow_saturates();
    test_run_windows_are_orderable();
    std::cout << "All time tests passed successfully!" << std::endl;
    return 0;
}
