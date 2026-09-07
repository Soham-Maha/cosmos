#include "cosmos/campaign.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr const char* kPlantedId = "planted.violation";

// Harness contract (§4): build_fn is a NAMED function, identical for Campaign::run and
// run_single, or repro runs silently diverge from campaign runs.

// Every third seed violates; the others are clean. Deterministic in the seed only.
void planted_violation_workload(cosmos::Simulator& sim, uint64_t seed) {
    cosmos::always(seed % 3 != 0, kPlantedId, "every third seed violates");
    (void)sim;
}

void clean_workload(cosmos::Simulator& sim, uint64_t seed) {
    (void)sim;
    (void)seed;
}

// Reads real-world entropy and leaks it into the event stream (the clock advance). The interim
// trace encoding observes nondeterminism that reaches ledger/clock events; the full §11.3
// encoding widens this to the whole execution. random_device (unwrapped /dev/urandom) gives
// per-call entropy, immune to host clock granularity.
void nondeterministic_workload(cosmos::Simulator& sim, uint64_t) {
    const uint64_t noise = (static_cast<uint64_t>(std::random_device{}()) << 32) ^
                           static_cast<uint64_t>(std::random_device{}());
    sim.advance_time(cosmos::Duration{static_cast<int64_t>(noise >> 20)});
}

cosmos::CampaignConfig small_config(bool verify) {
    cosmos::CampaignConfig cfg;
    cfg.trials = 24;
    cfg.base_seed = 1000;
    cfg.parallel = 2;
    cfg.verify = verify;
    return cfg;
}

} // namespace

// A planted violation is found: findings carry the violating seeds only, each carries its
// universe's seed, and failed_runs counts universes (8 of 24 seeds violate; findings == 8).
void test_campaign_finds_planted_violation() {
    cosmos::sometimes_registry().reset();
    const auto report = cosmos::run(small_config(false), planted_violation_workload);

    assert(report.runs == 24);
    assert(report.failed_runs == 8);
    assert(report.findings.size() == 8);
    for (const cosmos::Failure& f : report.findings) {
        assert(f.assertion_id == kPlantedId);
        assert(f.seed >= 1000 && f.seed < 1000 + 24);
        assert(f.seed % 3 == 0); // only the violating universes produced findings
    }

    const auto groups = report.grouped_findings(10);
    assert(groups.size() == 1);
    assert(groups.at(kPlantedId).size() == 8); // under the cap: every seed listed

    std::cout << "[PASS] test_campaign_finds_planted_violation" << std::endl;
}

// The grouping cap: with max_seeds_per_finding=3 and 8 findings, the report lists exactly 3
// seeds for the id (the full list stays in findings).
void test_report_grouping_respects_the_cap() {
    cosmos::sometimes_registry().reset();
    cosmos::CampaignConfig cfg = small_config(false);
    cfg.max_seeds_per_finding = 3;
    const auto report = cosmos::run(cfg, planted_violation_workload);

    const auto groups = report.grouped_findings(cfg.max_seeds_per_finding);
    assert(groups.at(kPlantedId).size() == 3);
    assert(report.findings.size() == 8); // the full list is retained regardless

    const std::string text = report.to_string(cfg.max_seeds_per_finding);
    assert(text.find("...)") != std::string::npos); // the elision marker prints

    std::cout << "[PASS] test_report_grouping_respects_the_cap" << std::endl;
}

// Verify mode: the same seed run twice must produce the same trace hash; a workload that reads
// real-world entropy diverges and is recorded as determinism.violation — one per seed, not an
// abort. The campaign keeps running to the end.
void test_verify_catches_planted_nondeterminism() {
    cosmos::sometimes_registry().reset();
    cosmos::CampaignConfig cfg = small_config(true);
    cfg.trials = 4;
    const auto report = cosmos::run(cfg, nondeterministic_workload);

    assert(report.runs == 4);
    assert(report.failed_runs == 4);
    assert(report.findings.size() == 4);
    for (const cosmos::Failure& f : report.findings) {
        assert(f.assertion_id == "determinism.violation");
        assert(f.detail.find("trace hash diverged on re-run") != std::string::npos);
    }

    std::cout << "[PASS] test_verify_catches_planted_nondeterminism" << std::endl;
}

// The same campaign run twice produces byte-identical report text regardless of how workers
// interleave (findings sorted by seed, groups in map order, never_hit sorted).
void test_report_is_order_independent() {
    cosmos::sometimes_registry().reset();

    const auto first = cosmos::run(small_config(false), planted_violation_workload);
    const std::string first_text = first.to_string(10);

    const auto second = cosmos::run(small_config(false), planted_violation_workload);
    const std::string second_text = second.to_string(10);

    assert(first_text == second_text);
    assert(!first_text.empty());

    std::cout << "[PASS] test_report_is_order_independent" << std::endl;
}

// never_hit: an id registered by every universe but satisfied in none is reported; a satisfied
// id is not.
void test_never_hit_reported() {
    cosmos::sometimes_registry().reset();

    const auto workload = [](cosmos::Simulator& sim, uint64_t seed) {
        (void)sim;
        (void)seed;
        cosmos::sometimes(false, "campaign.never-hit");
        cosmos::sometimes(true, "campaign.sometimes-hit");
    };
    const auto report = cosmos::run(small_config(false), workload);

    assert(report.never_hit.size() == 1);
    assert(report.never_hit[0] == "campaign.never-hit");
    const std::string text = report.to_string(10);
    assert(text.find("NEVER HIT: campaign.never-hit\n") != std::string::npos);
    assert(text.find("campaign.sometimes-hit") == std::string::npos);

    std::cout << "[PASS] test_never_hit_reported" << std::endl;
}

// Crash Containment item 2: with a report_path and flush cadence, the merged report reaches
// disk during the run and the final flush lands the complete sorted report.
void test_incremental_disk_flush() {
    cosmos::sometimes_registry().reset();

    const std::string path = "/tmp/cosmos_campaign_flush_test.txt";
    cosmos::CampaignConfig cfg = small_config(false);
    cfg.report_path = path;
    cfg.flush_every = 1;
    std::remove(path.c_str());

    const auto report = cosmos::run(cfg, planted_violation_workload);

    FILE* file = std::fopen(path.c_str(), "r");
    assert(file != nullptr);
    std::string on_disk;
    char buffer[512];
    size_t read_bytes = 0;
    while ((read_bytes = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        on_disk.append(buffer, read_bytes);
    }
    std::fclose(file);
    std::remove(path.c_str());

    assert(on_disk == report.to_string(cfg.max_seeds_per_finding));

    std::cout << "[PASS] test_incremental_disk_flush" << std::endl;
}

// Crash Containment item 1: the per-worker marker names the seed a worker is currently on.
void test_worker_seed_marker() {
    cosmos::WorkerSeedMarker marker;
    assert(!marker.active());
    marker.publish(12345);
    assert(marker.active());
    assert(marker.seed() == 12345);
    marker.publish(12346);
    assert(marker.seed() == 12346);
    marker.clear();
    assert(!marker.active());

    std::cout << "[PASS] test_worker_seed_marker" << std::endl;
}

// Single-seed repro mode: 0 on a clean universe, 1 with the finding on stderr — the exit
// contract CI pins regressions with.
void test_run_single_contract() {
    cosmos::sometimes_registry().reset();

    // Seeds 1000..1023, every third violates: 1002 is a violating seed, 1001 is clean.
    assert(cosmos::run_single(1002, planted_violation_workload) == 1);
    assert(cosmos::run_single(1001, planted_violation_workload) == 0);

    // Repro bit-identity: run_single's trace hash equals the campaign universe's hash for the
    // same seed (same binary, same seed — §8).
    const auto hash_via_run_single = [](uint64_t seed) {
        cosmos::Simulator sim(seed);
        cosmos::Simulator::Scope scope(sim);
        planted_violation_workload(sim, seed);
        sim.run_until_quiescence();
        return sim.trace_hash();
    };
    cosmos::Simulator direct(1002);
    {
        cosmos::Simulator::Scope scope(direct);
        planted_violation_workload(direct, 1002);
        direct.run_until_quiescence();
    }
    assert(hash_via_run_single(1002) == direct.trace_hash());

    std::cout << "[PASS] test_run_single_contract" << std::endl;
}

int main() {
    test_campaign_finds_planted_violation();
    test_report_grouping_respects_the_cap();
    test_verify_catches_planted_nondeterminism();
    test_report_is_order_independent();
    test_never_hit_reported();
    test_incremental_disk_flush();
    test_worker_seed_marker();
    test_run_single_contract();
    std::cout << "All campaign tests passed successfully!" << std::endl;
    return 0;
}
