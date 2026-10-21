#include "cosmos/cosmos.hpp"

#include <cassert>
#include <iostream>

namespace {

using cosmos::FaultClass;
using cosmos::FaultKind;
using cosmos::FaultRule;
using cosmos::NodeRegistry;
using cosmos::Simulator;
using cosmos::SiteId;

void test_unknown_nodes_and_wrong_states_are_refused() {
    NodeRegistry nodes{1};
    assert(nodes.node_count() == 1);
    assert(nodes.is_live(0));
    assert(!nodes.is_crashed(0));
    assert(!nodes.is_known(1));

    assert(!nodes.crash(1));  // unknown
    assert(!nodes.reboot(0)); // already live

    assert(nodes.crash(0));
    assert(nodes.is_crashed(0));
    assert(nodes.live_count() == 0);
    assert(nodes.crashed_count() == 1);

    assert(!nodes.crash(0)); // already crashed
    assert(nodes.reboot(0));
    assert(nodes.is_live(0));
    assert(!nodes.reboot(0));
    assert(nodes.crashed_count() == 0);

    std::cout << "[PASS] test_unknown_nodes_and_wrong_states_are_refused" << std::endl;
}

void test_counts_cannot_drift() {
    NodeRegistry nodes{4};
    for (cosmos::NodeId id = 0; id < 4u; ++id)
        assert(nodes.crash(id));
    assert(nodes.crashed_count() == 4);
    assert(nodes.live_count() == 0);
    for (cosmos::NodeId id = 0; id < 4u; ++id)
        assert(nodes.reboot(id));
    assert(nodes.crashed_count() == 0);
    assert(nodes.live_count() == 4);

    std::cout << "[PASS] test_counts_cannot_drift" << std::endl;
}

void test_shrinking_cannot_forget_a_crashed_node() {
    NodeRegistry nodes{3};
    assert(nodes.crash(2));
    assert(!nodes.set_node_count(2)); // node 2 is crashed, so the shrink would forget it
    assert(nodes.node_count() == 3);

    assert(nodes.reboot(2));
    assert(nodes.set_node_count(2));
    assert(nodes.node_count() == 2);
    assert(!nodes.is_known(2));

    // Growing marks the new nodes live, not crashed.
    assert(nodes.set_node_count(5));
    assert(nodes.is_live(4));
    assert(nodes.crashed_count() == 0);

    std::cout << "[PASS] test_shrinking_cannot_forget_a_crashed_node" << std::endl;
}

void test_simulator_refuses_double_crash() {
    Simulator sim;
    assert(sim.crash_node(0));
    assert(!sim.crash_node(0));
    assert(sim.reboot_node(0));
    assert(!sim.reboot_node(0));
    assert(!sim.crash_node(7));

    std::cout << "[PASS] test_simulator_refuses_double_crash" << std::endl;
}

// The reboot decision is an event site: it must reach the injector without a wrapper call, and it
// must land in both counters and the ledger like any other fire (§10.2).
void test_reboot_decision_reaches_the_injector() {
    Simulator sim;
    cosmos::FaultConfig cfg;
    cfg.enable_class(FaultClass::Storage);
    assert(cfg.activate_site(SiteId::torn_write));
    FaultRule tear;
    tear.rate = 1.0;
    assert(tear.outcomes.add(FaultKind::TornWrite, 1.0));
    assert(cfg.set_rule(SiteId::torn_write, tear));
    assert(sim.install_faults(cfg, 1).has_value());

    assert(sim.crash_node(0));
    assert(sim.reboot_node(0));

    const auto* injector = sim.injector_or_null();
    assert(injector != nullptr);
    assert(injector->eligible_calls(SiteId::torn_write) == 1);
    assert(injector->injections(SiteId::torn_write) == 1);

    const auto& ledger = injector->ledger();
    assert(ledger.size() == 1);
    assert(ledger[0].site == SiteId::torn_write);
    assert(ledger[0].outcome == FaultKind::TornWrite);
    assert(ledger[0].fault_class == FaultClass::Storage);

    std::cout << "[PASS] test_reboot_decision_reaches_the_injector" << std::endl;
}

} // namespace

int main() {
    test_unknown_nodes_and_wrong_states_are_refused();
    test_counts_cannot_drift();
    test_shrinking_cannot_forget_a_crashed_node();
    test_simulator_refuses_double_crash();
    test_reboot_decision_reaches_the_injector();
    std::cout << "All node registry tests passed successfully!" << std::endl;
    return 0;
}
