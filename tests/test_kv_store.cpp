/*
 * The Phase-4 exit gate for durability: an acknowledged write is a promise, and only the fsync
 * makes that promise survive a reboot.
 *
 * The store under test is a plain C application (examples/single_node/kv_store.c) reached through
 * the same -Wl,--wrap surface a real application would be. Nothing here calls into the durability
 * model directly: every case fails or passes on what the application can observe.
 */

#include "cosmos/cosmos.hpp"

#include "kv_store.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

constexpr uint64_t kSeed = 2024;

/* Key and value used by every case, so a failure names one scenario rather than one literal. */
constexpr const char* kKey = "alpha";
constexpr const char* kValue = "one";
/* Written after a torn tail has been cut off, to prove the log still accepts records. */
constexpr const char* kAfterKey = "beta";
constexpr const char* kAfterValue = "two";
/* The put a short write rejects: its record must leave no trace in the log. */
constexpr const char* kPartialKey = "gamma";
constexpr const char* kPartialValue = "three";

void must(bool ok) { assert(ok); }

/* mkdtemp per case: no case can replay another's wal.log, so the directory is also the isolation
   between universes. Created before a simulator exists, so its allocations are the host's. */
std::string make_dir(const char* tag) {
    std::string tmpl = "/tmp/cosmos_kv_" + std::string(tag) + "_XXXXXX";
    must(mkdtemp(tmpl.data()) != nullptr);
    return tmpl;
}

/* The oracle every case ends with: what the store acknowledged must read back byte for byte. It
   never asks whether a fault fired -- the fault is an input, and only the store's response to it
   can be wrong. */
bool holds(kv_store* store, const char* key, const char* expected) {
    char value[KV_VALUE_MAX];
    if (kv_get(store, key, value, sizeof value) != KV_OK) {
        return false;
    }
    return std::strcmp(value, expected) == 0;
}

/* A reboot that always tears the last synced sector: one outcome, rate 1, so the injection is a
   certainty rather than something the case hopes for. */
cosmos::FaultConfig torn_write_config() {
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Storage);
    must(cfg.activate_site(cosmos::SiteId::torn_write));
    cosmos::FaultRule rule;
    rule.rate = 1.0;
    must(rule.outcomes.add(cosmos::FaultKind::TornWrite, 1.0));
    must(cfg.set_rule(cosmos::SiteId::torn_write, rule));
    return cfg;
}

/* Only the first eligible write is made short: after that the budget is spent and the same store
   can append a whole record, which is what a repair has to leave room for. */
cosmos::FaultConfig short_write_config() {
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Storage);
    must(cfg.activate_site(cosmos::SiteId::write));
    cosmos::FaultRule rule;
    rule.rate = 1.0;
    rule.max_injections = 1;
    must(rule.outcomes.add(cosmos::FaultKind::ShortWrite, 1.0));
    must(cfg.set_rule(cosmos::SiteId::write, rule));
    return cfg;
}

struct TornOutcome {
    /* Must stay false: TornWrite is the modelled hardware lie -- the device acknowledged the fsync
       before the sector reached media -- so a surviving key would mean replay trusted that lie. */
    bool acked_key_survived = false;
    /* True is the expected answer: replay cut the damaged tail off and accepted a new record. */
    bool log_usable_afterwards = false;
    uint64_t torn_injections = 0;
};

/* One universe, start to finish, for the torn-sector case: durable put, crash, rebooting torn,
   then a fresh open that has to reject the damaged record and keep working afterwards. */
TornOutcome drive_torn(const std::string& dir) {
    cosmos::Simulator sim(kSeed);
    must(sim.install_faults(torn_write_config()).has_value());
    cosmos::Simulator::set_current(&sim);

    TornOutcome outcome;

    kv_store* store = kv_open(dir.c_str());
    must(store != nullptr);
    /* Durable first: the record reaches the host file and the model's durable size before the
       crash, which is exactly what a torn reboot damages. */
    must(kv_put(store, kKey, kValue, 1) == KV_OK);
    must(sim.crash_node(0));
    must(sim.reboot_node(0));
    kv_close(store);

    /* Replay stops at the zeroed sector, so the key it held is gone rather than half-restored. */
    store = kv_open(dir.c_str());
    must(store != nullptr);
    outcome.acked_key_survived = holds(store, kKey, kValue);

    /* The truncated log must accept the next record: recovery is only useful if the store keeps
       working, not merely if it refuses to lie about what it lost. */
    must(kv_put(store, kAfterKey, kAfterValue, 1) == KV_OK);
    kv_close(store);

    store = kv_open(dir.c_str());
    must(store != nullptr);
    outcome.log_usable_afterwards = holds(store, kAfterKey, kAfterValue);
    kv_close(store);

    auto* injector = sim.injector_or_null();
    must(injector != nullptr);
    outcome.torn_injections = injector->injections(cosmos::SiteId::torn_write);

    cosmos::Simulator::set_current(nullptr);
    return outcome;
}

/* The lost-write bug: the ack is in memory only, so the reboot takes it and replay cannot find it.
   This is the behaviour the store is allowed to have, and the reason kv_put has a durable flag. */
void test_unfsynced_ack_is_lost_across_a_reboot() {
    const std::string dir = make_dir("lost");

    cosmos::Simulator sim(kSeed);
    cosmos::Simulator::set_current(&sim);

    kv_store* store = kv_open(dir.c_str());
    must(store != nullptr);
    must(kv_put(store, kKey, kValue, 0) == KV_OK);
    must(holds(store, kKey, kValue)); /* acked: the table answers until the page cache does not */

    must(sim.crash_node(0));
    must(sim.reboot_node(0));
    kv_close(store);

    store = kv_open(dir.c_str());
    must(store != nullptr);
    must(!holds(store, kKey, kValue));
    kv_close(store);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_unfsynced_ack_is_lost_across_a_reboot" << std::endl;
}

/* The fixed half: the same sequence with the fsync the lost-write case omitted. */
void test_fsynced_put_survives_a_reboot() {
    const std::string dir = make_dir("kept");

    cosmos::Simulator sim(kSeed);
    cosmos::Simulator::set_current(&sim);

    kv_store* store = kv_open(dir.c_str());
    must(store != nullptr);
    must(kv_put(store, kKey, kValue, 1) == KV_OK);

    must(sim.crash_node(0));
    must(sim.reboot_node(0));
    kv_close(store);

    store = kv_open(dir.c_str());
    must(store != nullptr);
    must(holds(store, kKey, kValue));
    kv_close(store);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_fsynced_put_survives_a_reboot" << std::endl;
}

/* A failed put must leave the log exactly as it found it. A partial record left in place would make
   the next replay stop there and discard every record appended after it, including acked ones. */
void test_failed_put_leaves_no_partial_record() {
    const std::string dir = make_dir("short");

    /* The pre-existing key is written before any fault exists: the case starts from a log that
       already holds a durable record the partial append could cut away. */
    kv_store* store = kv_open(dir.c_str());
    must(store != nullptr);
    must(kv_put(store, kKey, kValue, 1) == KV_OK);
    kv_close(store);

    cosmos::Simulator sim(kSeed);
    must(sim.install_faults(short_write_config()).has_value());
    cosmos::Simulator::set_current(&sim);

    store = kv_open(dir.c_str());
    must(store != nullptr);
    /* The short write leaves half a record on disk; the store must cut it back rather than let the
       next append land behind it. */
    must(kv_put(store, kPartialKey, kPartialValue, 1) == KV_ERR);
    /* Same store, same log: a whole record must still fit where the partial one was removed. */
    must(kv_put(store, kAfterKey, kAfterValue, 1) == KV_OK);
    kv_close(store);

    auto* injector = sim.injector_or_null();
    must(injector != nullptr);
    must(injector->injections(cosmos::SiteId::write) == 1);

    store = kv_open(dir.c_str());
    must(store != nullptr);
    /* Exactly the acked keys: the rejected record neither survived nor took later ones with it. */
    must(holds(store, kKey, kValue));
    must(holds(store, kAfterKey, kAfterValue));
    must(!holds(store, kPartialKey, kPartialValue));
    kv_close(store);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_failed_put_leaves_no_partial_record" << std::endl;
}

/* A torn sector damages bytes, not the contract: the damaged record must not be answered, and the
   store must still be usable after recovery. */
void test_torn_tail_is_rejected_and_the_store_is_repaired() {
    const TornOutcome outcome = drive_torn(make_dir("torn"));

    /* Expected, not a discovery: TornWrite models a device that acknowledged fsync before the
       sector reached media (docs/design.md §8), so a surviving key is the bug, not the repair. */
    must(outcome.torn_injections == 1);
    must(!outcome.acked_key_survived);
    must(outcome.log_usable_afterwards);

    std::cout << "[PASS] test_torn_tail_is_rejected_and_the_store_is_repaired" << std::endl;
}

/* Same universe seed, same sequence, same verdicts: a campaign can only trust a failure it can
   reproduce, which is what makes the torn case worth a fix rather than a flake. */
void test_the_torn_case_reproduces_under_one_seed() {
    const TornOutcome first = drive_torn(make_dir("det_a"));
    const TornOutcome second = drive_torn(make_dir("det_b"));

    must(first.torn_injections == second.torn_injections);
    must(first.acked_key_survived == second.acked_key_survived);
    must(first.log_usable_afterwards == second.log_usable_afterwards);

    std::cout << "[PASS] test_the_torn_case_reproduces_under_one_seed" << std::endl;
}

} // namespace

int main() {
    test_unfsynced_ack_is_lost_across_a_reboot();
    test_fsynced_put_survives_a_reboot();
    test_failed_put_leaves_no_partial_record();
    test_torn_tail_is_rejected_and_the_store_is_repaired();
    test_the_torn_case_reproduces_under_one_seed();
    std::cout << "All kv_store durability tests passed successfully!" << std::endl;
    return 0;
}
