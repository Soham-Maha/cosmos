#include "cosmos/faults.hpp"
#include "cosmos/random.hpp"
#include "cosmos/simulator.hpp"

#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::KnobScale;
using cosmos::KnobSpec;
using cosmos::SiteId;

namespace {

constexpr uint32_t kNodes = 1;

void must(bool ok) { assert(ok); }

// The seed the campaign would hand this universe's Simulator (§6.4 level 0).
uint64_t universe(uint64_t index) { return cosmos::universe_seed(4242, index); }

uint64_t swarm_stream(uint64_t seed) {
    return cosmos::stream_seed(seed, cosmos::StreamDomain::Swarm);
}

FaultConfig sample_stream(uint64_t seed) {
    cosmos::Rng rng(seed);
    return FaultConfig::sample(rng);
}

FaultConfig sample_universe(uint64_t index) { return sample_stream(swarm_stream(universe(index))); }

FaultConfig sample_with_stream(uint64_t seed, std::bitset<cosmos::kFaultClassCount> enabled) {
    cosmos::Rng rng(seed);
    return FaultConfig::sample_with(rng, enabled);
}

FaultConfig sample_knobs_stream(uint64_t seed, std::span<const KnobSpec> specs) {
    cosmos::Rng rng(seed);
    return FaultConfig::sample(rng, specs);
}

void test_every_sample_validates() {
    for (uint64_t index = 0; index < 10000; ++index) {
        must(sample_universe(index).validate(kNodes).has_value());
    }
    std::cout << "[PASS] test_every_sample_validates" << std::endl;
}

// Doc §14's swarm coverage: no class and no site may be unreachable from the sampler.
void test_every_class_and_site_is_reachable_in_a_campaign() {
    std::array<size_t, cosmos::kFaultClassCount> class_hits{};
    std::array<size_t, cosmos::kSiteCount> site_hits{};
    for (uint64_t index = 0; index < 1000; ++index) {
        const FaultConfig cfg = sample_universe(index);
        for (size_t cls = 0; cls < cosmos::kFaultClassCount; ++cls) {
            if (cfg.enabled.test(cls)) ++class_hits[cls];
        }
        for (size_t slot = 0; slot < cosmos::kSiteCount; ++slot) {
            if (cfg.activated_sites.test(slot)) ++site_hits[slot];
        }
        // One sub-stream per class (Rule 8): independent draws cannot all land on one value.
        double first_rate = -1.0;
        bool rates_differ = false;
        size_t rules = 0;
        for (const std::optional<cosmos::FaultRule>& rule : cfg.rules) {
            if (!rule) continue;
            if (rules == 0) first_rate = rule->rate;
            rates_differ = rates_differ || rule->rate != first_rate;
            ++rules;
        }
        must(rules < 2 || rates_differ);
    }
    for (size_t cls = 0; cls < cosmos::kFaultClassCount; ++cls) {
        std::cout << "  " << cosmos::name_of(static_cast<FaultClass>(cls)) << " enabled in "
                  << class_hits[cls] << "/1000" << std::endl;
        must(class_hits[cls] > 0);
    }
    for (size_t slot = 0; slot < cosmos::kSiteCount; ++slot) {
        std::cout << "  " << cosmos::name_of(cosmos::site_at_slot(slot)) << " activated in "
                  << site_hits[slot] << "/1000" << std::endl;
        must(site_hits[slot] > 0);
    }
    std::cout << "[PASS] test_every_class_and_site_is_reachable_in_a_campaign" << std::endl;
}

// Rule 8, measured: one class's dimensions come from that class's own sub-stream.
void test_flipping_a_class_does_not_shift_another() {
    bool saw_network_activation = false;
    std::bitset<cosmos::kFaultClassCount> all_on;
    all_on.set();
    std::bitset<cosmos::kFaultClassCount> without_network = all_on;
    without_network.reset(static_cast<size_t>(FaultClass::Network));

    for (uint64_t index = 0; index < 20; ++index) {
        const uint64_t stream = swarm_stream(universe(index));
        const FaultConfig on = sample_with_stream(stream, all_on);
        const FaultConfig off = sample_with_stream(stream, without_network);

        for (size_t slot = 0; slot < cosmos::kWrapperSiteCount; ++slot) {
            if (cosmos::class_of(cosmos::site_at_slot(slot)) == FaultClass::Network) {
                // Flipping the bit off must empty the class out; nothing else changes.
                must(!off.activated_sites.test(slot));
                must(!off.rules[slot].has_value());
                if (on.activated_sites.test(slot)) {
                    saw_network_activation = true;
                    must(on.rules[slot].has_value());
                }
                continue;
            }
            must(on.activated_sites.test(slot) == off.activated_sites.test(slot));
            must(on.rules[slot] == off.rules[slot]);
        }
    }
    // Without this the loop above would also pass if the flip changed nothing at all.
    must(saw_network_activation);
    std::cout << "[PASS] test_flipping_a_class_does_not_shift_another" << std::endl;
}

void test_the_same_universe_seed_samples_the_same_config() {
    for (uint64_t index = 0; index < 100; ++index) {
        must(sample_universe(index) == sample_universe(index));
    }
    std::cout << "[PASS] test_the_same_universe_seed_samples_the_same_config" << std::endl;
}

void test_universe_seeds_sample_different_configs() {
    std::vector<FaultConfig> configs;
    for (uint64_t index = 0; index < 100; ++index) {
        configs.push_back(sample_universe(index));
    }
    size_t distinct = 0;
    bool saw_a_disabled_class = false;
    for (size_t i = 0; i < configs.size(); ++i) {
        bool duplicate = false;
        for (size_t j = 0; j < i; ++j) {
            duplicate = duplicate || configs[i] == configs[j];
        }
        if (!duplicate) ++distinct;
        saw_a_disabled_class =
            saw_a_disabled_class || configs[i].enabled.count() < cosmos::kFaultClassCount;
    }
    must(distinct >= 95);
    must(saw_a_disabled_class);
    std::cout << "[PASS] test_universe_seeds_sample_different_configs (" << distinct
              << "/100 distinct)" << std::endl;
}

void test_sampling_reads_only_the_swarm_stream() {
    const uint64_t seed = universe(7);

    // The accessor hands out the Swarm domain stream, not the Fault or the User one.
    cosmos::Simulator named(seed);
    cosmos::Rng expected(swarm_stream(seed));
    must(named.swarm_rng().next() == expected.next());

    // Sampling must not consume the application's randomness (or any other stream).
    cosmos::Simulator untouched(seed);
    cosmos::Rng user_before = untouched.user_rng();
    const FaultConfig cfg = FaultConfig::sample(untouched.swarm_rng());
    must(untouched.user_rng().next() == user_before.next());

    // One config sampled before install_faults and one after: the fault streams cannot tell.
    cosmos::Simulator first(seed);
    must(first.install_faults(cfg).has_value());
    cosmos::Simulator second(seed);
    must(second.install_faults(cfg).has_value());
    must(sample_stream(swarm_stream(seed)) == cfg);
    for (int call = 0; call < 200; ++call) {
        must(first.injector_or_null()->decide(FaultClass::Memory, SiteId::malloc) ==
             second.injector_or_null()->decide(FaultClass::Memory, SiteId::malloc));
    }
    std::cout << "[PASS] test_sampling_reads_only_the_swarm_stream" << std::endl;
}

constexpr KnobSpec kCacheCapacity{1, 1, 64, KnobScale::Logarithmic, "COSMOS_CACHE_CAPACITY"};
constexpr KnobSpec kRetryBudget{2, 0, 8, KnobScale::Linear, "COSMOS_RETRY_BUDGET"};

void test_knob_values_stay_inside_their_declared_range() {
    const KnobSpec specs[] = {kCacheCapacity, kRetryBudget};
    for (uint64_t index = 0; index < 1000; ++index) {
        const uint64_t stream = swarm_stream(universe(index));
        const FaultConfig cfg = sample_knobs_stream(stream, specs);
        must(cfg.knobs.size() == 2);
        must(cfg.knobs[0].id < cfg.knobs[1].id);
        must(cfg.knobs[0].value >= kCacheCapacity.low && cfg.knobs[0].value <= kCacheCapacity.high);
        must(cfg.knobs[1].value >= kRetryBudget.low && cfg.knobs[1].value <= kRetryBudget.high);
        must(cfg.knob_value(kCacheCapacity.id) == cfg.knobs[0].value);
        must(!cfg.knob_value(99).has_value());
        // A knob the harness never declared is never sampled.
        must(!sample_stream(stream).knob_value(kCacheCapacity.id).has_value());
    }
    std::cout << "[PASS] test_knob_values_stay_inside_their_declared_range" << std::endl;
}

// §6.4: a knob must not shift a class's draws, and one knob's range must not shift another's.
void test_knobs_do_not_shift_a_class_or_another_knob() {
    const KnobSpec specs[] = {kCacheCapacity, kRetryBudget};
    const KnobSpec narrow[] = {{1, 1, 10, KnobScale::Linear, nullptr}, kRetryBudget};

    for (uint64_t index = 0; index < 100; ++index) {
        const uint64_t stream = swarm_stream(universe(index));
        const FaultConfig bare = sample_stream(stream);
        const FaultConfig with_knobs = sample_knobs_stream(stream, specs);
        must(with_knobs.enabled == bare.enabled);
        must(with_knobs.activated_sites == bare.activated_sites);
        must(with_knobs.rules == bare.rules);

        const FaultConfig narrow_cfg = sample_knobs_stream(stream, narrow);
        must(narrow_cfg.knob_value(kRetryBudget.id) == with_knobs.knob_value(kRetryBudget.id));
    }
    std::cout << "[PASS] test_knobs_do_not_shift_a_class_or_another_knob" << std::endl;
}

} // namespace

int main() {
    test_every_sample_validates();
    test_every_class_and_site_is_reachable_in_a_campaign();
    test_flipping_a_class_does_not_shift_another();
    test_the_same_universe_seed_samples_the_same_config();
    test_universe_seeds_sample_different_configs();
    test_sampling_reads_only_the_swarm_stream();
    test_knob_values_stay_inside_their_declared_range();
    test_knobs_do_not_shift_a_class_or_another_knob();
    std::cout << "All swarm tests passed successfully!" << std::endl;
    return 0;
}
