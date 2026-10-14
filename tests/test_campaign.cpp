#include "cosmos/campaign.hpp"
#include "cosmos/scenario.hpp"

#include "broken_cache.h"

#include <algorithm>
#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kBaseSeed = 1;
constexpr int kKeys = 20;
constexpr double kOomRate = 0.05;

void must(bool ok) { assert(ok); }

const cosmos::Finding* find_finding(const cosmos::CampaignReport& report, const std::string& id) {
    for (const cosmos::Finding& finding : report.findings) {
        if (finding.id == id) return &finding;
    }
    return nullptr;
}

bool lists_never_hit(const cosmos::CampaignReport& report, const std::string& id) {
    for (const std::string& listed : report.never_hit) {
        if (listed == id) return true;
    }
    return false;
}

// Built once, outside any universe: formatting here would add allocations the eligible count is not
// expecting, and the fixture's exact eligible count is what makes its failure reproducible.
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

// The oracle: anything the application said it stored must still be there. A fault is an input, and
// only the application's response to it can be wrong.
bool every_acknowledged_key_survived(const Keys& keys, const Fixture& fixture) {
    if (fixture.cache == nullptr) return false;
    for (int i = 0; i < kKeys; ++i) {
        if (!fixture.put_ok[i]) continue;
        const char* got = cache_get(fixture.cache, keys.key[i]);
        if (got == nullptr || std::strcmp(got, keys.value[i]) != 0) return false;
    }
    return true;
}

cosmos::FaultPlan oom_rate(double rate) {
    cosmos::FaultPlan plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    must(plan.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    rule.rate = rate;
    must(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    must(plan.set_rule(cosmos::SiteId::malloc, std::move(rule)));
    return plan;
}

// One universe's result: its report, plus the capacity the application actually saw.
struct CacheRun {
    cosmos::ScenarioReport report{};
    size_t observed_capacity = 0;
};

void put_every_key(const Keys& keys, Fixture& fixture) {
    fixture.cache = cache_create();
    if (fixture.cache == nullptr) return;
    for (int i = 0; i < kKeys; ++i) {
        fixture.put_ok[i] = cache_put(fixture.cache, keys.key[i], keys.value[i]) == CACHE_OK;
    }
}

// §17.2's scenario, shaped for a campaign: the universe comes in, the report goes out. Every cache
// campaign shape shares it, so the oracle and the counters stay comparable across tests.
CacheRun run_cache_once(cosmos::Simulator& sim, cosmos::FaultPlan plan,
                        int64_t expected_low = CACHE_CAPACITY,
                        int64_t expected_high = CACHE_CAPACITY) {
    static const Keys keys;
    Fixture fixture;

    auto scenario = cosmos::Scenario::on(sim, std::move(plan));
    must(scenario.has_value());

    scenario->run([&] { put_every_key(keys, fixture); });
    scenario->quiesce();
    const size_t observed = fixture.cache != nullptr ? cache_capacity(fixture.cache) : 0;
    scenario->check(
        "no-acknowledged-key-lost", [&] { return every_acknowledged_key_survived(keys, fixture); },
        "cache_put reported success for every key");
    // §4.3's delivery contract: what the application saw is what the harness handed it. A universe
    // whose cache never allocated has nothing to say about the capacity it was given.
    scenario->check(
        "knob-was-delivered",
        [&] {
            return fixture.cache == nullptr || (observed >= static_cast<size_t>(expected_low) &&
                                                observed <= static_cast<size_t>(expected_high));
        },
        "capacity=" + std::to_string(observed));

    size_t fires = 0;
    for (size_t slot = 0; slot < cosmos::kSiteCount; ++slot) {
        fires += scenario->report().injections[slot];
    }
    scenario->note_covered("any-fault-fired", fires > 0);
    // §11.4: the campaign can see the rare path being reached rather than inferring it.
    scenario->note_covered("capacity-below-default", observed < CACHE_CAPACITY);

    CacheRun run;
    run.observed_capacity = observed;
    run.report = scenario->report();
    fixture.release();
    return run;
}

// The fixed-rate shape: only the universe seed varies, which is what the repro identity needs.
cosmos::ScenarioReport run_cache_universe(cosmos::Simulator& sim, uint64_t) {
    return run_cache_once(sim, oom_rate(kOomRate)).report;
}

// §17.3's shape: one sampled config per universe seed (P4-S2).
cosmos::ScenarioReport run_swarm_universe(cosmos::Simulator& sim, uint64_t) {
    return run_cache_once(sim, cosmos::FaultConfig::sample(sim.swarm_rng())).report;
}

// The capacity is the fixture's tunable, so the knob's declared range is the fixture's limit.
constexpr cosmos::KnobSpec kCapacityKnob{1, 1, CACHE_CAPACITY, cosmos::KnobScale::Logarithmic,
                                         CACHE_CAPACITY_ENV};
const cosmos::KnobSpec kKnobSpecs[] = {kCapacityKnob};

// Sampled config plus a sampled knob, delivered to the application through the environment.
cosmos::ScenarioReport run_knob_universe(cosmos::Simulator& sim, uint64_t) {
    const cosmos::FaultConfig plan = cosmos::FaultConfig::sample(sim.swarm_rng(), kKnobSpecs);
    cosmos::ScenarioReport report;
    cosmos::with_knob_env(plan, kKnobSpecs, [&] {
        report = run_cache_once(sim, plan, kCapacityKnob.low, kCapacityKnob.high).report;
    });
    return report;
}

cosmos::CampaignReport run_cache_campaign(uint64_t trials, unsigned parallel) {
    cosmos::CampaignConfig config;
    config.trials = trials;
    config.base_seed = kBaseSeed;
    config.parallel = parallel;
    config.program = "test_campaign";
    return cosmos::Campaign::run(config, run_cache_universe);
}

// The campaign's whole reason to exist: it finds the bug across seeds and names one to re-run.
void test_campaign_finds_the_broken_app_failure() {
    const cosmos::CampaignReport report = run_cache_campaign(1000, 1);
    must(report.runs == 1000);
    must(report.failed_runs > 0);

    const cosmos::Finding* finding = find_finding(report, "no-acknowledged-key-lost");
    must(finding != nullptr);
    must(finding->seed != 0);
    must(finding->seed == cosmos::universe_seed(kBaseSeed, finding->index));
    must(finding->universes_failed > 0);
    // A bug that fires in most universes is not a flaky-looking one.
    must(!finding->rare);
    must(!finding->ledger.empty());

    std::cout << "[PASS] test_campaign_finds_the_broken_app_failure" << std::endl;
}

// The repro identity is (base_seed, index); the printed seed must be enough on its own, because
// that is all an application's --seed flag receives.
void test_the_named_seed_alone_reproduces_the_failure() {
    const cosmos::CampaignReport report = run_cache_campaign(200, 1);
    const cosmos::Finding* finding = find_finding(report, "no-acknowledged-key-lost");
    must(finding != nullptr);

    cosmos::Simulator sim(finding->seed);
    const cosmos::ScenarioReport alone = run_cache_universe(sim, finding->index);
    must(!alone.no_check_failed);
    const cosmos::CheckResult* check = nullptr;
    for (const cosmos::CheckResult& result : alone.checks) {
        if (result.id == "no-acknowledged-key-lost") check = &result;
    }
    must(check != nullptr);
    must(!check->passed);

    std::cout << "[PASS] test_the_named_seed_alone_reproduces_the_failure" << std::endl;
}

// Campaign indexing must go through universe_seed(), never campaign_seed + 1, + 2, ... . The body's
// captured state does not survive a worker process, so the check is made where it can be reported.
void test_every_universe_gets_its_own_derived_seed() {
    std::vector<uint64_t> sorted;
    for (uint64_t index = 0; index < 512; ++index)
        sorted.push_back(cosmos::universe_seed(kBaseSeed, index));
    std::sort(sorted.begin(), sorted.end());
    must(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    cosmos::CampaignConfig config;
    config.trials = 64;
    config.base_seed = kBaseSeed;
    config.parallel = 1;
    const cosmos::CampaignReport report =
        cosmos::Campaign::run(config, [](cosmos::Simulator& sim, uint64_t index) {
            auto scenario = cosmos::Scenario::on(sim, cosmos::FaultPlan{});
            must(scenario.has_value());
            scenario->run([] {});
            scenario->quiesce();
            scenario->check("seed-matches-index",
                            [&] { return sim.seed() == cosmos::universe_seed(kBaseSeed, index); });
            return scenario->report();
        });

    must(report.runs == 64);
    must(report.findings.empty());

    std::cout << "[PASS] test_every_universe_gets_its_own_derived_seed" << std::endl;
}

// A universe that throws fails its own run and nothing else.
void test_throwing_universe_fails_only_itself() {
    cosmos::CampaignConfig config;
    config.trials = 32;
    config.base_seed = kBaseSeed;
    config.parallel = 1;

    const cosmos::CampaignReport report =
        cosmos::Campaign::run(config, [](cosmos::Simulator& sim, uint64_t index) {
            auto scenario = cosmos::Scenario::on(sim, cosmos::FaultPlan{});
            must(scenario.has_value());
            if (index == 7) throw std::runtime_error("universe exploded");
            scenario->run([] {});
            scenario->quiesce();
            scenario->check("always-holds", [] { return true; });
            return scenario->report();
        });

    must(report.runs == 32);
    must(report.failed_runs == 1);
    const cosmos::Finding* finding = find_finding(report, cosmos::kThrewUniverseId);
    must(finding != nullptr);
    must(finding->index == 7);
    must(finding->detail == "universe exploded");

    std::cout << "[PASS] test_throwing_universe_fails_only_itself" << std::endl;
}

// Liveness ids that no universe reached are reported apart from failures, never as findings.
void test_never_hit_stays_out_of_findings() {
    cosmos::CampaignConfig config;
    config.trials = 8;
    config.base_seed = kBaseSeed;
    config.parallel = 1;

    const cosmos::CampaignReport report =
        cosmos::Campaign::run(config, [](cosmos::Simulator& sim, uint64_t) {
            auto scenario = cosmos::Scenario::on(sim, cosmos::FaultPlan{});
            must(scenario.has_value());
            scenario->run([] {});
            scenario->quiesce();
            scenario->check("always-holds", [] { return true; });
            scenario->note_covered("unreached-path", false);
            scenario->note_covered("reached-path", true);
            return scenario->report();
        });

    must(report.findings.empty());
    must(report.failed_runs == 0);
    must(lists_never_hit(report, "unreached-path"));
    must(!lists_never_hit(report, "reached-path"));

    std::cout << "[PASS] test_never_hit_stays_out_of_findings" << std::endl;
}

// The flag promises a trace-hash check we cannot deliver until P5-S4, so it is refused rather than
// accepted and quietly unhonoured.
void test_verify_is_refused_rather_than_ignored() {
    cosmos::CampaignConfig config;
    config.trials = 1000;
    config.verify = true;

    const cosmos::CampaignReport report =
        cosmos::Campaign::run(config, [](cosmos::Simulator&, uint64_t) -> cosmos::ScenarioReport {
            must(false && "a refused campaign must not run any universe");
            return {};
        });

    must(report.refused);
    must(report.runs == 0);
    must(report.findings.size() == 1);
    must(report.findings[0].id == cosmos::kVerifyRefusedId);

    std::cout << "[PASS] test_verify_is_refused_rather_than_ignored" << std::endl;
}

void test_report_prints_a_repro_line() {
    const cosmos::CampaignReport report = run_cache_campaign(200, 1);
    std::ostringstream out;
    cosmos::print_campaign_report(out, report);
    const std::string text = out.str();
    must(text.find("FINDING \"no-acknowledged-key-lost\"") != std::string::npos);
    must(text.find("test_campaign --seed ") != std::string::npos);
    must(text.find("no-acknowledged-key-lost") != std::string::npos);

    std::cout << "[PASS] test_report_prints_a_repro_line" << std::endl;
}

// The dashboard's contract: findings and never_hit in separate sections, counters paired per site,
// and every emitted string properly escaped so a detail can contain quotes and newlines.
void test_json_report_is_well_formed() {
    const cosmos::CampaignReport report = run_cache_campaign(200, 1);
    std::ostringstream out;
    cosmos::print_campaign_json(out, report);
    const std::string json = out.str();

    must(json.find("\"runs\": 200") != std::string::npos);
    must(json.find("\"never_hit\": [") != std::string::npos);
    must(json.find("\"sites\": {") != std::string::npos);
    must(json.find("\"repro\"") == std::string::npos);
    must(json.find("\"seed\": ") != std::string::npos);
    must(json.front() == '{');
    must(json.back() == '\n');
    must(std::count(json.begin(), json.end(), '{') == std::count(json.begin(), json.end(), '}'));
    must(std::count(json.begin(), json.end(), '[') == std::count(json.begin(), json.end(), ']'));

    // Escaping: a detail carrying a quote and a newline must not break the string it lives in.
    cosmos::CampaignReport awkward;
    cosmos::Finding finding;
    finding.id = "quoted\"id";
    finding.detail = "line one\nline \"two\"";
    awkward.findings.push_back(finding);
    std::ostringstream escaped_out;
    cosmos::print_campaign_json(escaped_out, awkward);
    const std::string escaped = escaped_out.str();
    must(escaped.find("quoted\\\"id") != std::string::npos);
    must(escaped.find("line one\\nline \\\"two\\\"") != std::string::npos);
    must(escaped.find("line one\nline") == std::string::npos);

    std::cout << "[PASS] test_json_report_is_well_formed" << std::endl;
}

// The campaign must not be able to tell how its universes were distributed, so everything a caller
// can read has to match -- not just the findings set.
bool same_findings(const cosmos::CampaignReport& first, const cosmos::CampaignReport& second) {
    if (first.runs != second.runs || first.failed_runs != second.failed_runs) return false;
    if (first.crashed_runs != second.crashed_runs) return false;
    if (first.findings.size() != second.findings.size()) return false;
    for (size_t i = 0; i < first.findings.size(); ++i) {
        const cosmos::Finding& a = first.findings[i];
        const cosmos::Finding& b = second.findings[i];
        if (a.id != b.id || a.index != b.index || a.seed != b.seed) return false;
        if (a.detail != b.detail || a.rare != b.rare) return false;
        if (a.universes_failed != b.universes_failed || a.ledger != b.ledger) return false;
    }
    if (first.never_hit != second.never_hit) return false;
    if (first.eligible_calls != second.eligible_calls) return false;
    return first.injections == second.injections;
}

void test_parallel_and_sequential_agree_exactly() {
    const cosmos::CampaignReport sequential = run_cache_campaign(300, 1);
    const cosmos::CampaignReport parallel = run_cache_campaign(300, 4);
    must(sequential.runs == 300);
    must(same_findings(sequential, parallel));
    // Guard the guard: an empty report would satisfy the comparison for the wrong reason.
    must(!sequential.findings.empty());

    std::cout << "[PASS] test_parallel_and_sequential_agree_exactly" << std::endl;
}

// A universe that dies takes its own run down and nothing else: the campaign finishes, and the
// crash is attributed to the seed that caused it.
void test_crashing_universe_fails_only_itself(unsigned parallel) {
    cosmos::CampaignConfig config;
    config.trials = 16;
    config.base_seed = kBaseSeed;
    config.parallel = parallel;

    const cosmos::CampaignReport report =
        cosmos::Campaign::run(config, [](cosmos::Simulator& sim, uint64_t index) {
            auto scenario = cosmos::Scenario::on(sim, cosmos::FaultPlan{});
            must(scenario.has_value());
            if (index == 5) std::raise(SIGABRT);
            scenario->run([] {});
            scenario->quiesce();
            scenario->check("always-holds", [] { return true; });
            return scenario->report();
        });

    must(report.runs == 16);
    must(report.crashed_runs == 1);
    must(report.failed_runs == 1);
    const cosmos::Finding* finding = find_finding(report, cosmos::kCrashedUniverseId);
    must(finding != nullptr);
    must(finding->index == 5);
    must(finding->seed == cosmos::universe_seed(kBaseSeed, 5));

    std::cout << "[PASS] test_crashing_universe_fails_only_itself(parallel=" << parallel << ")"
              << std::endl;
}

// The phase gate's 10k-seed campaign, and the only test that drives the worker channel at scale.
void test_a_ten_thousand_universe_swarm_campaign_completes() {
    cosmos::CampaignConfig config;
    config.trials = 10000;
    config.base_seed = kBaseSeed;
    config.parallel = 4;
    config.program = "test_campaign";

    const cosmos::CampaignReport report = cosmos::Campaign::run(config, run_swarm_universe);

    must(report.runs == 10000);
    must(report.crashed_runs == 0);
    must(report.failed_runs > 0);
    // One assertion, one finding: the harness's own verdicts stay out of the findings list.
    must(report.findings.size() == 1);
    must(report.findings[0].id == "no-acknowledged-key-lost");
    must(report.eligible_calls[cosmos::site_slot(cosmos::SiteId::malloc)] > 0);
    must(report.injections[cosmos::site_slot(cosmos::SiteId::malloc)] > 0);
    must(!lists_never_hit(report, "any-fault-fired"));

    std::cout << "[PASS] test_a_ten_thousand_universe_swarm_campaign_completes" << std::endl;
}

// §4.3 end to end: the sampled knob reaches the application, varies across universes, and the
// environment is put back afterwards.
void test_a_sampled_knob_reaches_the_application() {
    cosmos::CampaignConfig config;
    config.trials = 200;
    config.base_seed = kBaseSeed;
    config.parallel = 1;
    config.program = "test_campaign";

    const cosmos::CampaignReport report = cosmos::Campaign::run(config, run_knob_universe);

    must(report.runs == 200);
    must(find_finding(report, "knob-was-delivered") == nullptr);
    // The extreme end of the declared range was delivered at least once.
    must(!lists_never_hit(report, "capacity-below-default"));
    must(std::getenv(CACHE_CAPACITY_ENV) == nullptr);

    std::cout << "[PASS] test_a_sampled_knob_reaches_the_application" << std::endl;
}

} // namespace

int main() {
    test_campaign_finds_the_broken_app_failure();
    test_the_named_seed_alone_reproduces_the_failure();
    test_every_universe_gets_its_own_derived_seed();
    test_throwing_universe_fails_only_itself();
    test_never_hit_stays_out_of_findings();
    test_verify_is_refused_rather_than_ignored();
    test_report_prints_a_repro_line();
    test_json_report_is_well_formed();
    test_parallel_and_sequential_agree_exactly();
    test_crashing_universe_fails_only_itself(1);
    test_crashing_universe_fails_only_itself(4);
    test_a_ten_thousand_universe_swarm_campaign_completes();
    test_a_sampled_knob_reaches_the_application();
    std::cout << "All campaign tests passed successfully!" << std::endl;
    return 0;
}
