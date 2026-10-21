#include "cosmos/campaign.hpp"
#include "cosmos/minimize.hpp"
#include "cosmos/scenario.hpp"

#include "broken_cache.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kBaseSeed = 11;
constexpr uint64_t kSeed = 0x5CA1AB1E;

constexpr int kKeys = 20;
constexpr double kOomRate = 0.15;

void must(bool ok) { assert(ok); }

const cosmos::Finding* find_finding(const cosmos::CampaignReport& report, const std::string& id) {
    for (const cosmos::Finding& finding : report.findings) {
        if (finding.id == id) return &finding;
    }
    return nullptr;
}

std::vector<uint64_t> seqs(const std::vector<cosmos::TraceEntry>& entries) {
    std::vector<uint64_t> out;
    for (const cosmos::TraceEntry& entry : entries)
        out.push_back(entry.seq);
    return out;
}

// ---- a deterministic three-site failure: the check needs malloc and calloc to fail together ----

cosmos::FaultRule triggered(cosmos::FaultKind kind) {
    cosmos::FaultRule rule;
    rule.rate = 0.0;
    must(rule.outcomes.add(kind, 1.0));
    rule.fire_on_eligible_call = 1;
    return rule;
}

cosmos::FaultConfig three_site_plan() {
    cosmos::FaultConfig plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    must(plan.activate_site(cosmos::SiteId::malloc));
    must(plan.activate_site(cosmos::SiteId::calloc));
    must(plan.activate_site(cosmos::SiteId::realloc));
    must(plan.set_rule(cosmos::SiteId::malloc, triggered(cosmos::FaultKind::OutOfMemory)));
    must(plan.set_rule(cosmos::SiteId::calloc, triggered(cosmos::FaultKind::OutOfMemory)));
    must(plan.set_rule(cosmos::SiteId::realloc, triggered(cosmos::FaultKind::OutOfMemory)));
    return plan;
}

void three_site_body(cosmos::Scenario& scenario) {
    bool malloc_failed = false;
    bool calloc_failed = false;
    scenario.run([&] {
        void* block = std::malloc(16);
        malloc_failed = block == nullptr;
        std::free(block);
        void* table = std::calloc(2, 8);
        calloc_failed = table == nullptr;
        std::free(table);
        void* grown = std::realloc(nullptr, 16);
        std::free(grown);
    });
    scenario.quiesce();
    scenario.check("double-oom", [&] { return !(malloc_failed && calloc_failed); });
}

cosmos::ScenarioReport three_site_universe(cosmos::Simulator& sim, uint64_t) {
    auto scenario = cosmos::Scenario::on(sim, three_site_plan());
    must(scenario.has_value());
    three_site_body(*scenario);
    return scenario->report();
}

// ---- shared helpers -------------------------------------------------------

cosmos::FaultTrace record_trace(uint64_t seed, const cosmos::FaultConfig& plan, uint64_t capacity,
                                void (*body)(cosmos::Scenario&)) {
    auto scenario = cosmos::Scenario::create(
        seed, plan, 1, cosmos::TraceOptions{.enabled = true, .capacity = capacity});
    must(scenario.has_value());
    body(*scenario);
    const auto* injector = scenario->simulator().injector_or_null();
    must(injector != nullptr);
    return injector->trace();
}

// True when the named check passed in a replay that keeps only `kept` fires.
bool replay_check(const cosmos::FaultTrace& source, const cosmos::FaultConfig& plan,
                  std::vector<uint64_t> kept, const std::string& id,
                  void (*body)(cosmos::Scenario&)) {
    auto scenario = cosmos::Scenario::replay(
        source, cosmos::TraceFilter::only_faults(source, std::move(kept)), plan);
    must(scenario.has_value());
    body(*scenario);
    for (const cosmos::CheckResult& check : scenario->report().checks) {
        if (check.id == id) return check.passed;
    }
    must(false && "the replay did not register the named check");
    return false;
}

// ---- P5-S3 checkpoints ----------------------------------------------------

// The bug needs both OOMs; the realloc fire is noise and must be dropped.
void test_a_campaign_finding_minimizes_to_two_essential_faults() {
    cosmos::CampaignConfig campaign;
    campaign.trials = 4;
    campaign.base_seed = kBaseSeed;
    campaign.parallel = 1;
    campaign.program = "test_minimize";

    const cosmos::CampaignReport found = cosmos::Campaign::run(campaign, three_site_universe);
    const cosmos::Finding* finding = find_finding(found, "double-oom");
    must(finding != nullptr);
    must(finding->seed != 0);

    const cosmos::FaultConfig plan = three_site_plan();
    const cosmos::FaultTrace source = record_trace(finding->seed, plan, 64, three_site_body);
    must(source.size() == 3);
    must(source.dropped() == 0);

    cosmos::MinimizeConfig config;
    config.plan = plan;
    config.finding_id = "double-oom";
    config.program = "test_minimize";

    const cosmos::MinimizeReport report = cosmos::minimize(source, config, three_site_body);

    must(!report.refused);
    must(report.candidates.size() == 3);
    must(report.kept.size() == 2);
    must(report.passes == 2);
    must(report.replays == 6); // baseline + pass 1 (3) + pass 2 (2)
    must(report.crashed_trials == 0);
    must(seqs(report.kept) == std::vector<uint64_t>({1, 2}));

    // The kept set still fails, and removing either entry alone makes the check pass.
    must(!replay_check(source, plan, seqs(report.kept), "double-oom", three_site_body));
    for (uint64_t seq : seqs(report.kept)) {
        std::vector<uint64_t> without = seqs(report.kept);
        without.erase(std::find(without.begin(), without.end(), seq));
        must(replay_check(source, plan, without, "double-oom", three_site_body));
    }

    std::cout << "[PASS] test_a_campaign_finding_minimizes_to_two_essential_faults" << std::endl;
}

void test_report_prints_the_minimal_set_and_repro_command() {
    const cosmos::FaultConfig plan = three_site_plan();
    const cosmos::FaultTrace source = record_trace(kSeed, plan, 64, three_site_body);

    cosmos::MinimizeConfig config;
    config.plan = plan;
    config.finding_id = "double-oom";
    config.program = "test_minimize";
    const cosmos::MinimizeReport report = cosmos::minimize(source, config, three_site_body);
    must(!report.refused);

    std::ostringstream out;
    cosmos::print_minimize_report(out, report);
    const std::string text = out.str();
    must(text.find("Minimal fault set for \"double-oom\"") != std::string::npos);
    must(text.find("1-minimal") != std::string::npos);
    must(text.find("site=malloc") != std::string::npos);
    must(text.find("site=calloc") != std::string::npos);
    must(text.find("Repro: test_minimize --seed " + std::to_string(kSeed) + " --only-faults=1,2") !=
         std::string::npos);
    must(text.find("2 entries, nothing else") != std::string::npos);

    std::cout << "[PASS] test_report_prints_the_minimal_set_and_repro_command" << std::endl;
}

void crash_on_suppression(cosmos::Scenario& scenario) {
    bool malloc_failed = false;
    bool calloc_failed = false;
    scenario.run([&] {
        void* block = std::malloc(16);
        malloc_failed = block == nullptr;
        std::free(block);
        void* table = std::calloc(2, 8);
        calloc_failed = table == nullptr;
        std::free(table);
        void* grown = std::realloc(nullptr, 16);
        std::free(grown);
    });
    scenario.quiesce();
    const auto* injector = scenario.simulator().injector_or_null();
    must(injector != nullptr);
    // A suppression trial dies instead of reporting: the minimizer must read that as "essential".
    if (injector->replay_withheld() > 0) std::abort();
    scenario.check("double-oom", [&] { return !(malloc_failed && calloc_failed); });
}

void test_a_crashing_suppression_is_kept_not_treated_as_reproduction() {
    const cosmos::FaultConfig plan = three_site_plan();
    const cosmos::FaultTrace source = record_trace(kSeed, plan, 64, crash_on_suppression);

    must(source.size() == 3);

    cosmos::MinimizeConfig config;
    config.plan = plan;
    config.finding_id = "double-oom";
    const cosmos::MinimizeReport report = cosmos::minimize(source, config, crash_on_suppression);

    must(!report.refused);
    must(report.candidates.size() == 3);
    must(report.kept.size() == 3);
    must(report.passes == 1);
    must(report.replays == 4); // baseline + one crash per candidate
    must(report.crashed_trials == 3);

    std::cout << "[PASS] test_a_crashing_suppression_is_kept_not_treated_as_reproduction"
              << std::endl;
}

void test_a_trace_that_does_not_reproduce_is_refused() {
    const cosmos::FaultConfig plan = three_site_plan();
    const cosmos::FaultTrace source = record_trace(kSeed, plan, 64, three_site_body);

    cosmos::MinimizeConfig config;
    config.plan = plan;
    config.finding_id = "not-registered";
    const cosmos::MinimizeReport report = cosmos::minimize(source, config, three_site_body);

    must(report.refused);
    must(report.reason.find("not-registered") != std::string::npos);
    must(report.replays == 1);

    std::cout << "[PASS] test_a_trace_that_does_not_reproduce_is_refused" << std::endl;
}

void test_a_truncated_trace_is_refused() {
    const cosmos::FaultConfig plan = three_site_plan();
    const cosmos::FaultTrace source = record_trace(kSeed, plan, 2, three_site_body);
    must(source.dropped() > 0);

    cosmos::MinimizeConfig config;
    config.plan = plan;
    config.finding_id = "double-oom";
    const cosmos::MinimizeReport report = cosmos::minimize(source, config, three_site_body);

    must(report.refused);
    must(report.reason.find("truncated") != std::string::npos);
    must(report.replays == 0);

    std::cout << "[PASS] test_a_truncated_trace_is_refused" << std::endl;
}

// ---- the P4 fixture, end to end -------------------------------------------

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

struct CacheFixture {
    Cache* cache = nullptr;
    bool put_ok[kKeys] = {};

    void release() {
        cache_destroy(cache);
        cache = nullptr;
    }
};

bool every_acknowledged_key_survived(const Keys& keys, const CacheFixture& fixture) {
    if (fixture.cache == nullptr) return false;
    for (int i = 0; i < kKeys; ++i) {
        if (!fixture.put_ok[i]) continue;
        const char* got = cache_get(fixture.cache, keys.key[i]);
        if (got == nullptr || std::strcmp(got, keys.value[i]) != 0) return false;
    }
    return true;
}

cosmos::FaultConfig oom_plan() {
    cosmos::FaultConfig plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    must(plan.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    rule.rate = kOomRate;
    must(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    must(plan.set_rule(cosmos::SiteId::malloc, std::move(rule)));
    return plan;
}

void cache_body(cosmos::Scenario& scenario) {
    static const Keys keys;
    CacheFixture fixture;
    scenario.run([&] {
        fixture.cache = cache_create();
        if (fixture.cache == nullptr) return;
        for (int i = 0; i < kKeys; ++i) {
            fixture.put_ok[i] = cache_put(fixture.cache, keys.key[i], keys.value[i]) == CACHE_OK;
        }
    });
    scenario.quiesce();
    scenario.check("no-acknowledged-key-lost",
                   [&] { return every_acknowledged_key_survived(keys, fixture); });
    fixture.release();
}

cosmos::ScenarioReport cache_universe(cosmos::Simulator& sim, uint64_t) {
    auto scenario = cosmos::Scenario::on(sim, oom_plan());
    must(scenario.has_value());
    cache_body(*scenario);
    return scenario->report();
}

void test_the_broken_cache_campaign_finding_minimizes() {
    cosmos::CampaignConfig campaign;
    campaign.trials = 200;
    campaign.base_seed = 1;
    campaign.parallel = 1;
    campaign.program = "test_minimize";

    const cosmos::CampaignReport found = cosmos::Campaign::run(campaign, cache_universe);
    const cosmos::Finding* finding = find_finding(found, "no-acknowledged-key-lost");
    must(finding != nullptr);

    const cosmos::FaultConfig plan = oom_plan();
    const cosmos::FaultTrace source = record_trace(finding->seed, plan, 4096, cache_body);
    must(source.size() != 0);

    cosmos::MinimizeConfig config;
    config.plan = plan;
    config.finding_id = "no-acknowledged-key-lost";
    config.program = "test_minimize";
    const cosmos::MinimizeReport report = cosmos::minimize(source, config, cache_body);

    must(!report.refused);
    // One surviving OOM still loses its key, so a single fire is enough to reproduce.
    must(report.kept.size() == 1);
    if (report.candidates.size() > 1) must(report.kept.size() < report.candidates.size());
    must(!replay_check(source, plan, seqs(report.kept), "no-acknowledged-key-lost", cache_body));
    must(replay_check(source, plan, {}, "no-acknowledged-key-lost", cache_body));

    std::cout << "[PASS] test_the_broken_cache_campaign_finding_minimizes" << std::endl;
}

} // namespace

int main() {
    test_a_campaign_finding_minimizes_to_two_essential_faults();
    test_report_prints_the_minimal_set_and_repro_command();
    test_a_crashing_suppression_is_kept_not_treated_as_reproduction();
    test_a_trace_that_does_not_reproduce_is_refused();
    test_a_truncated_trace_is_refused();
    test_the_broken_cache_campaign_finding_minimizes();
    std::cout << "All minimize tests passed successfully!" << std::endl;
    return 0;
}
