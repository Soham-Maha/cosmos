#include "cosmos/scenario.hpp"

#include "broken_cache.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr uint64_t kSeed = 8421;
constexpr int kKeys = 20;

// cache_create allocates once and each new key allocates once, so the app makes exactly this many
// eligible calls. Nothing else in the workload allocates, which is what makes fail_on_call exact.
constexpr uint64_t kEligibleCalls = 1 + kKeys;
constexpr uint64_t kFailOnCall = 5;

void must(bool ok) { assert(ok); }

// Built before the universe starts: formatting inside the workload would add allocations the
// eligible count is not expecting.
struct Keys {
    char key[kKeys][CACHE_KEY_MAX];
    char value[kKeys][CACHE_VALUE_MAX];

    Keys() {
        for (int i = 0; i < kKeys; ++i) {
            std::snprintf(key[i], CACHE_KEY_MAX, "key%02d", i);
            std::snprintf(value[i], CACHE_VALUE_MAX, "value%02d", i);
        }
    }
};

struct Fixture {
    Cache* cache = nullptr;
    bool put_ok[kKeys] = {};

    void release() {
        cache_destroy(cache);
        cache = nullptr;
    }
};

// The oracle: anything the application said it stored must still be there. It never asks whether a
// fault fired -- a fault is an input, and only the app's response to it can be wrong.
bool every_acknowledged_key_survived(const Keys& keys, const Fixture& fixture) {
    if (fixture.cache == nullptr) return false;
    for (int i = 0; i < kKeys; ++i) {
        if (!fixture.put_ok[i]) continue;
        const char* got = cache_get(fixture.cache, keys.key[i]);
        if (got == nullptr || std::strcmp(got, keys.value[i]) != 0) return false;
    }
    return true;
}

cosmos::FaultPlan oom_at(std::optional<uint64_t> fail_on_call) {
    cosmos::FaultPlan plan;
    if (!fail_on_call.has_value()) return plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    must(plan.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    must(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    rule.fire_on_eligible_call = fail_on_call;
    must(plan.set_rule(cosmos::SiteId::malloc, std::move(rule)));
    return plan;
}

struct Outcome {
    bool passed = false;
    std::string printed;
    uint64_t eligible = 0;
    uint64_t injections = 0;
    size_t active_allocations = 0;
};

// One universe, start to finish: the shape §17.2 walks through, minus the distributed parts. The
// cache is torn down only after the oracle has read it -- an oracle inspects the state the run left
// behind, so releasing it before quiesce would leave nothing to judge.
Outcome drive(std::optional<uint64_t> fail_on_call) {
    static const Keys keys;
    Fixture fixture;

    auto scenario = cosmos::Scenario::create(kSeed, oom_at(fail_on_call));
    must(scenario.has_value());

    scenario->run([&] {
        fixture.cache = cache_create();
        if (fixture.cache == nullptr) return;
        for (int i = 0; i < kKeys; ++i) {
            fixture.put_ok[i] = cache_put(fixture.cache, keys.key[i], keys.value[i]) == CACHE_OK;
        }
    });

    scenario->quiesce();
    scenario->check(
        "no-acknowledged-key-lost", [&] { return every_acknowledged_key_survived(keys, fixture); },
        "cache_put reported success for every key");
    scenario->note_covered(
        "oom-path-exercised",
        scenario->report().injections[cosmos::site_slot(cosmos::SiteId::malloc)] > 0);

    Outcome outcome;
    outcome.passed = scenario->passed();
    outcome.eligible = scenario->report().eligible_calls[cosmos::site_slot(cosmos::SiteId::malloc)];
    outcome.injections = scenario->report().injections[cosmos::site_slot(cosmos::SiteId::malloc)];
    outcome.active_allocations = scenario->report().active_allocations;

    std::ostringstream out;
    cosmos::print_report(out, *scenario);
    outcome.printed = out.str();

    fixture.release();
    return outcome;
}

#ifdef BROKEN_CACHE_CHECKS_MALLOC
constexpr bool kAppIsFixed = true;
#else
constexpr bool kAppIsFixed = false;
#endif

void test_same_seed_reproduces_across_ten_runs() {
    const Outcome first = drive(kFailOnCall);
    for (int i = 0; i < 9; ++i) {
        const Outcome again = drive(kFailOnCall);
        assert(again.passed == first.passed);
        assert(again.printed == first.printed);
        assert(again.eligible == first.eligible);
        assert(again.injections == first.injections);
    }
    assert(first.eligible == kEligibleCalls);
    assert(first.injections == 1);
    assert(first.passed == kAppIsFixed);

    std::cout << "[PASS] test_same_seed_reproduces_across_ten_runs" << std::endl;
}

// The report necessarily comes from the broken run: the fixed one has no failure to describe.
void test_failure_report_names_the_seed_and_the_fault() {
    const Outcome outcome = drive(kFailOnCall);

    // Same fault in both variants; only the response differs. Asserted on both sides so this test
    // does not depend on a sibling to establish that the rule actually fired.
    assert(outcome.eligible == kEligibleCalls);
    assert(outcome.injections == 1);

    if (kAppIsFixed) {
        assert(outcome.passed);
        assert(outcome.printed.find("PASSED  seed = 8421") != std::string::npos);
        assert(outcome.printed.find("Fault ledger:") == std::string::npos);
    } else {
        assert(!outcome.passed);
        assert(outcome.printed.find("FAILED: \"no-acknowledged-key-lost\"") != std::string::npos);
        assert(outcome.printed.find("seed    = 8421") != std::string::npos);
        assert(outcome.printed.find("Fault ledger:") != std::string::npos);
        assert(outcome.printed.find("site=malloc") != std::string::npos);
        assert(outcome.printed.find("OutOfMemory") != std::string::npos);
        assert(outcome.printed.find("eligible #5") != std::string::npos);
    }

    std::cout << "[PASS] test_failure_report_names_the_seed_and_the_fault" << std::endl;
}

// The earthquake guard: with no fault injected the app must be clean, or the finding above was a
// pre-existing bug rather than something the injector caused.
void test_faults_disabled_passes() {
    const Outcome outcome = drive(std::nullopt);

    assert(outcome.passed);
    assert(outcome.eligible == 0);
    assert(outcome.injections == 0);
    assert(outcome.printed.find("NOT COVERED: \"oom-path-exercised\"") != std::string::npos);

    std::cout << "[PASS] test_faults_disabled_passes" << std::endl;
}

// Armed but out of reach, rather than a seed that happens not to fire: the rule is live, the site
// is counted, nothing is injected, and the clean run is recorded as uncovered rather than hidden.
void test_armed_but_never_fired_is_recorded_not_hidden() {
    const Outcome outcome = drive(kEligibleCalls + 100);

    assert(outcome.passed);
    assert(outcome.eligible == kEligibleCalls);
    assert(outcome.injections == 0);
    assert(outcome.printed.find("NOT COVERED: \"oom-path-exercised\"") != std::string::npos);

    std::cout << "[PASS] test_armed_but_never_fired_is_recorded_not_hidden" << std::endl;
}

// check_no_leaks is opt-in precisely because this fixture is the counter-example: a cache still
// holding its entries at quiesce is doing its job, not leaking.
void test_leak_check_is_opt_in() {
    static const Keys keys;
    Fixture fixture;

    auto scenario = cosmos::Scenario::create(kSeed, oom_at(std::nullopt));
    must(scenario.has_value());
    scenario->run([&] {
        fixture.cache = cache_create();
        for (int i = 0; i < kKeys; ++i) {
            fixture.put_ok[i] = cache_put(fixture.cache, keys.key[i], keys.value[i]) == CACHE_OK;
        }
    });
    scenario->quiesce();

    assert(scenario->report().active_allocations == static_cast<size_t>(1 + kKeys));
    assert(!scenario->check_no_leaks("no-leaks"));
    assert(!scenario->passed());

    std::ostringstream out;
    cosmos::print_report(out, *scenario);
    assert(out.str().find("FAILED: \"no-leaks\"") != std::string::npos);
    assert(out.str().find("active_allocations=21") != std::string::npos);

    fixture.release();
    std::cout << "[PASS] test_leak_check_is_opt_in" << std::endl;
}

void test_leak_check_passes_after_teardown() {
    static const Keys keys;
    Fixture fixture;

    auto scenario = cosmos::Scenario::create(kSeed, oom_at(std::nullopt));
    must(scenario.has_value());
    scenario->run([&] {
        fixture.cache = cache_create();
        for (int i = 0; i < kKeys; ++i) {
            fixture.put_ok[i] = cache_put(fixture.cache, keys.key[i], keys.value[i]) == CACHE_OK;
        }
        fixture.release();
    });
    scenario->quiesce();

    assert(scenario->report().active_allocations == 0);
    assert(scenario->check_no_leaks("no-leaks"));
    assert(scenario->passed());

    std::cout << "[PASS] test_leak_check_passes_after_teardown" << std::endl;
}

// The leak verdict must describe the universe as the workload left it, not as the oracles found
// it: a check that allocates while judging would otherwise change the answer of a later one.
void test_leak_verdict_ignores_oracle_allocations() {
    static const Keys keys;
    Fixture fixture;

    auto scenario = cosmos::Scenario::create(kSeed, oom_at(std::nullopt));
    must(scenario.has_value());
    scenario->run([&] {
        fixture.cache = cache_create();
        for (int i = 0; i < kKeys; ++i) {
            fixture.put_ok[i] = cache_put(fixture.cache, keys.key[i], keys.value[i]) == CACHE_OK;
        }
        fixture.release();
    });
    scenario->quiesce();

    void* held = nullptr;
    assert(scenario->check("oracle-that-allocates", [&] {
        held = malloc(64);
        return held != nullptr;
    }));
    assert(scenario->simulator().heap().active_count() == 1);

    assert(scenario->check_no_leaks("no-leaks"));
    assert(scenario->passed());

    free(held);
    std::cout << "[PASS] test_leak_verdict_ignores_oracle_allocations" << std::endl;
}

// The snapshot describes the state the workload left, so it must be taken before the harness's own
// bookkeeping runs. Only observable while the universe is current -- otherwise quiesce()'s
// allocations pass through untracked -- and only where operator new reaches the wrappers, which is
// what test_broken_app_static is linked for.
void test_leak_snapshot_precedes_harness_bookkeeping() {
    auto scenario = cosmos::Scenario::create(kSeed, oom_at(std::nullopt));
    must(scenario.has_value());

    // Deliberately vacuous: quiesce() records a lifecycle failure, and recording it allocates.
    cosmos::Simulator::set_current(&scenario->simulator());
    scenario->quiesce();
    cosmos::Simulator::set_current(nullptr);

    assert(scenario->report().active_allocations == 0);
    assert(!scenario->passed());

    std::cout << "[PASS] test_leak_snapshot_precedes_harness_bookkeeping" << std::endl;
}

} // namespace

int main() {
    std::cout << (kAppIsFixed ? "-- fixed application --" : "-- broken application --")
              << std::endl;
    test_same_seed_reproduces_across_ten_runs();
    test_failure_report_names_the_seed_and_the_fault();
    test_faults_disabled_passes();
    test_armed_but_never_fired_is_recorded_not_hidden();
    test_leak_check_is_opt_in();
    test_leak_check_passes_after_teardown();
    test_leak_verdict_ignores_oracle_allocations();
    test_leak_snapshot_precedes_harness_bookkeeping();
    std::cout << "All broken-app tests passed successfully!" << std::endl;
    return 0;
}
