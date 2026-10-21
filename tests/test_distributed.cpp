#include "cosmos/campaign.hpp"
#include "cosmos/cosmos.hpp"
#include "cosmos/minimize.hpp"
#include "cosmos/scenario.hpp"
#include "replicated_kv.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <pthread.h>
#include <string>
#include <time.h>

namespace {

using cosmos::Campaign;
using cosmos::CampaignConfig;
using cosmos::CrashNode;
using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::FaultTrace;
using cosmos::MinimizeConfig;
using cosmos::MinimizeReport;
using cosmos::NodeUniverseFn;
using cosmos::Partition;
using cosmos::Scenario;
using cosmos::ScenarioReport;
using cosmos::Simulator;
using cosmos::SiteId;
using cosmos::Time;
using cosmos::TraceOptions;

constexpr uint64_t kSeed = 7311;
constexpr uint32_t kNodes = RKV_MAX_NODES;
constexpr const char* kKey = "k";
constexpr const char* kValue = "v";
constexpr const char* kFinding = "acked-write-is-replicated";

int g_client_rc = 99;

struct NodeArg {
    int node = 0;
};

void* node_thread(void* raw) {
    NodeArg* arg = static_cast<NodeArg*>(raw);
    (void)rkv_node_main(arg->node, kNodes, RKV_BASE_PORT);
    return nullptr;
}

// The §17 plan: partition the primary from one backup and kill the other at the same instant.
FaultConfig distributed_plan() {
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Process);
    cfg.enable_class(FaultClass::Network);
    cfg.max_crashed_nodes = 1;
    cfg.min_healthy_quorum = 0;
    assert(cfg.activate_site(SiteId::crash_node));
    assert(cfg.activate_site(SiteId::partition));
    cfg.scheduled_episodes.push_back(
        {Time{1'000'000}, Partition{{0}, {1}}, cosmos::Duration{10'000'000}});
    cfg.scheduled_episodes.push_back({Time{1'000'000}, CrashNode{2}, cosmos::Duration{5'000'000}});
    return cfg;
}

// Drives one universe: start the cluster, write during the fault window, heal, stop, settle, check.
void run_cluster(Scenario& scenario) {
    rkv_reset();
    g_client_rc = 99;
    Simulator& sim = scenario.simulator();
    NodeArg args[kNodes];

    scenario.run([&] {
        pthread_t threads[kNodes] = {};
        for (int n = 1; n < static_cast<int>(kNodes); ++n) {
            args[n].node = n;
            assert(sim.spawn_node(static_cast<cosmos::NodeId>(n), &threads[n], node_thread,
                                  &args[n]) == 0);
        }
        args[0].node = 0;
        assert(sim.spawn_node(0, &threads[0], node_thread, &args[0]) == 0);

        timespec settle{};
        settle.tv_nsec = 1'000'000L;
        nanosleep(&settle, nullptr); // t=1ms: scheduled episodes fire before this wake
        g_client_rc = rkv_client_put(RKV_BASE_PORT, kKey, kValue);

        auto* injector = sim.injector_or_null();
        if (injector != nullptr) injector->heal_all();
        nanosleep(&settle, nullptr); // let the healed follower settle
        rkv_shutdown();              // wakes every blocked fiber deterministically

        for (int n = 0; n < static_cast<int>(kNodes); ++n) {
            assert(pthread_join(threads[n], nullptr) == 0);
        }
    });

    scenario.quiesce();

    const bool acked = g_client_rc == 0;
    const bool on_backup = rkv_node_has(1, kKey, kValue) != 0 || rkv_node_has(2, kKey, kValue) != 0;
    scenario.check(kFinding, [acked, on_backup] { return !acked || on_backup; });
}

ScenarioReport run_universe(Simulator& sim, uint64_t index, uint32_t node_count) {
    (void)index;
    auto scenario = Scenario::on(sim, distributed_plan(), node_count);
    assert(scenario.has_value());
    run_cluster(*scenario);
    return scenario->report();
}

FaultConfig liveness_plan() {
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Process);
    cfg.enable_class(FaultClass::Network);
    cfg.max_crashed_nodes = 1;
    cfg.min_healthy_quorum = 0;
    assert(cfg.activate_site(SiteId::crash_node));
    return cfg;
}

void run_cluster_under_liveness(Scenario& scenario) {
    rkv_reset();
    g_client_rc = 99;
    Simulator& sim = scenario.simulator();
    NodeArg args[kNodes];

    scenario.run([&] {
        pthread_t threads[kNodes] = {};
        for (int n = 1; n < static_cast<int>(kNodes); ++n) {
            args[n].node = n;
            assert(sim.spawn_node(static_cast<cosmos::NodeId>(n), &threads[n], node_thread,
                                  &args[n]) == 0);
        }
        args[0].node = 0;
        assert(sim.spawn_node(0, &threads[0], node_thread, &args[0]) == 0);

        timespec settle{};
        settle.tv_nsec = 1'000'000L;
        nanosleep(&settle, nullptr);
        auto* injector = sim.injector_or_null();
        assert(injector != nullptr);
        assert(injector->crash_node(1, cosmos::Duration{50'000'000}).has_value());
        assert(injector->enter_liveness_mode({0})); // node 1 is non-core: persistent
        nanosleep(&settle, nullptr);
        g_client_rc = rkv_client_put(RKV_BASE_PORT, kKey, kValue);
        nanosleep(&settle, nullptr);
        rkv_shutdown();
        for (int n = 0; n < static_cast<int>(kNodes); ++n) {
            assert(pthread_join(threads[n], nullptr) == 0);
        }
    });

    scenario.quiesce();
    const bool acked = g_client_rc == 0;
    const bool on_backup = rkv_node_has(2, kKey, kValue) != 0;
    scenario.check(kFinding, [acked, on_backup] { return !acked || on_backup; });
}

// No faults: the write is acknowledged and both backups hold it.
void test_cluster_replicates_without_faults() {
    Simulator sim(kSeed);
    auto scenario = Scenario::create(kSeed, FaultConfig{}, kNodes);
    assert(scenario.has_value());
    run_cluster(*scenario);
    assert(g_client_rc == 0);
    assert(rkv_node_has(1, kKey, kValue));
    assert(rkv_node_has(2, kKey, kValue));
    assert(scenario->report().passed());
    std::cout << "[PASS] test_cluster_replicates_without_faults" << std::endl;
}

void test_liveness_mode_keeps_progress_on_the_fixture() {
    Simulator sim(kSeed);
    auto scenario = Scenario::create(kSeed, liveness_plan(), kNodes);
    assert(scenario.has_value());
    run_cluster_under_liveness(*scenario);
    assert(g_client_rc == 0);
    assert(rkv_node_has(2, kKey, kValue));
    assert(scenario->report().passed());
    const auto* injector = scenario->simulator().injector_or_null();
    assert(injector != nullptr && injector->active_episodes() == 0);
    std::cout << "[PASS] test_liveness_mode_keeps_progress_on_the_fixture" << std::endl;
}

#ifndef RKV_FIXED
// The bug: an acked write reaches no backup when the partition drops one copy and the crash kills
// the other. The quiesce-time oracle is what detects it.
void test_partition_plus_crash_loses_an_acked_write() {
    Simulator sim(kSeed);
    auto scenario = Scenario::create(kSeed, distributed_plan(), kNodes);
    assert(scenario.has_value());
    run_cluster(*scenario);
    assert(g_client_rc == 0);
    assert(!rkv_node_has(1, kKey, kValue));
    assert(!rkv_node_has(2, kKey, kValue));
    assert(!scenario->report().passed());

    const auto* injector = scenario->simulator().injector_or_null();
    assert(injector != nullptr);
    bool saw_partition = false;
    bool saw_crash = false;
    for (const auto& row : injector->ledger()) {
        if (row.site == SiteId::partition) saw_partition = true;
        if (row.site == SiteId::crash_node) saw_crash = true;
    }
    assert(saw_partition && saw_crash);
    for (const auto& row : injector->ledger()) {
        assert(row.status != cosmos::DecisionStatus::Skipped); // the plan stayed inside the model
    }
    assert(injector->active_episodes() == 0);
    assert(injector->quiescing());
    std::cout << "[PASS] test_partition_plus_crash_loses_an_acked_write" << std::endl;
}

// The campaign must surface the same failure as a finding, deterministically under verify.
void test_campaign_finds_the_replication_bug() {
    CampaignConfig config;
    config.trials = 8;
    config.parallel = 1;
    config.node_count = kNodes;
    config.verify = true;
    config.program = "replicated_kv";
    const auto report = Campaign::run(config, NodeUniverseFn(run_universe));
    bool found = false;
    for (const auto& finding : report.findings) {
        if (finding.id == kFinding) found = true;
        assert(finding.id != "cosmos.campaign.verify");
    }
    assert(found);
    std::cout << "[PASS] test_campaign_finds_the_replication_bug" << std::endl;
}

// Minimization keeps exactly the two episodes: neither alone loses the write.
void test_minimizer_keeps_the_essential_pair() {
    FaultTrace trace;
    {
        Simulator sim(kSeed);
        auto scenario = Scenario::create(kSeed, distributed_plan(), kNodes, TraceOptions{true, 64});
        assert(scenario.has_value());
        run_cluster(*scenario);
        assert(!scenario->report().passed());
        trace = scenario->simulator().injector_or_null()->trace();
    }
    assert(trace.size() == 2);

    MinimizeConfig config;
    config.plan = distributed_plan();
    config.finding_id = kFinding;
    config.node_count = kNodes;
    config.program = "replicated_kv";
    const MinimizeReport report =
        cosmos::minimize(trace, config, [](Scenario& scenario) { run_cluster(scenario); });
    assert(!report.refused);
    assert(report.kept.size() == 2);
    bool partition = false;
    bool crash = false;
    for (const auto& entry : report.kept) {
        if (entry.site == SiteId::partition) partition = true;
        if (entry.site == SiteId::crash_node) crash = true;
    }
    assert(partition && crash);
    std::cout << "[PASS] test_minimizer_keeps_the_essential_pair" << std::endl;
}
#else
// The fixed build refuses the write instead of acknowledging one no backup holds.
void test_fixed_build_refuses_an_unreplicated_write() {
    Simulator sim(kSeed);
    auto scenario = Scenario::create(kSeed, distributed_plan(), kNodes);
    assert(scenario.has_value());
    run_cluster(*scenario);
    assert(g_client_rc != 0);
    assert(scenario->report().passed());
    std::cout << "[PASS] test_fixed_build_refuses_an_unreplicated_write" << std::endl;
}
#endif

} // namespace

int main() {
    test_cluster_replicates_without_faults();
    test_liveness_mode_keeps_progress_on_the_fixture();
#ifndef RKV_FIXED
    test_partition_plus_crash_loses_an_acked_write();
    test_campaign_finds_the_replication_bug();
    test_minimizer_keeps_the_essential_pair();
#else
    test_fixed_build_refuses_an_unreplicated_write();
#endif
    std::cout << "All distributed tests passed successfully!" << std::endl;
    return 0;
}
