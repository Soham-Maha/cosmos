#include "cosmos/cosmos.hpp"
#include "cosmos/ledger_print.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace {

using cosmos::DecisionStatus;
using cosmos::Duration;
using cosmos::EpisodeId;
using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::FaultTrace;
using cosmos::Simulator;
using cosmos::SiteId;
using cosmos::SkipReason;
using cosmos::Time;
using cosmos::TraceFilter;
using cosmos::TraceOptions;

constexpr uint32_t kNodes = 3;
constexpr uint64_t kSeed = 4242;

FaultConfig base_config() {
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Process);
    cfg.enable_class(FaultClass::Network);
    cfg.max_crashed_nodes = 2;
    cfg.min_healthy_quorum = 1;
    assert(cfg.activate_site(SiteId::crash_node));
    assert(cfg.activate_site(SiteId::partition));
    assert(cfg.activate_site(SiteId::pause_node));
    return cfg;
}

cosmos::Duration ms(int64_t value) { return Duration{value * 1'000'000}; }
cosmos::Time at_ms(int64_t value) { return Time{value * 1'000'000}; }

const cosmos::LedgerEntry& last_row(const Simulator& sim) {
    const auto* injector = sim.injector_or_null();
    assert(injector != nullptr && injector->ledger().size() > 0);
    return injector->ledger()[injector->ledger().size() - 1];
}

// 1. A started crash heals itself at now + until_reboot and flips the node back.
void test_crash_episode_heals_automatically() {
    Simulator sim(kSeed);
    assert(sim.install_faults(base_config(), kNodes).has_value());
    auto* injector = sim.injector_or_null();
    assert(injector != nullptr);

    const auto id = injector->crash_node(1, ms(2));
    assert(id.has_value());
    assert(sim.nodes().is_crashed(1));
    assert(injector->active_episodes() == 1);
    assert(last_row(sim).site == SiteId::crash_node);
    assert(last_row(sim).status == DecisionStatus::Fired);

    sim.scheduler().run_until_quiescence();
    assert(sim.now().ns == 2'000'000);
    assert(sim.nodes().is_live(1));
    assert(injector->active_episodes() == 0);
    assert(injector->ledger().size() == 2);
    std::cout << "[PASS] test_crash_episode_heals_automatically" << std::endl;
}

// 2. An early heal withdraws the scheduled heal; time never reaches the original deadline.
void test_heal_cancels_the_auto_heal() {
    Simulator sim(kSeed);
    assert(sim.install_faults(base_config(), kNodes).has_value());
    auto* injector = sim.injector_or_null();

    const auto id = injector->crash_node(1, ms(10));
    assert(id.has_value());
    injector->heal(*id);
    assert(sim.nodes().is_live(1));
    assert(injector->active_episodes() == 0);

    sim.scheduler().run_until_quiescence();
    assert(sim.now().ns == 0);
    assert(injector->ledger().size() == 2);
    assert(injector->ledger()[1].status == DecisionStatus::Fired);
    std::cout << "[PASS] test_heal_cancels_the_auto_heal" << std::endl;
}

// 3. max_crashed_nodes refuses the second start and records the reason.
void test_max_crashed_nodes_refuses() {
    FaultConfig cfg = base_config();
    cfg.max_crashed_nodes = 1;
    Simulator sim(kSeed);
    assert(sim.install_faults(cfg, kNodes).has_value());
    auto* injector = sim.injector_or_null();

    assert(injector->crash_node(0, ms(5)).has_value());
    assert(!injector->crash_node(1, ms(5)).has_value());
    assert(sim.nodes().crashed_count() == 1);
    assert(last_row(sim).status == DecisionStatus::Skipped);
    assert(last_row(sim).reason == SkipReason::MaxCrashedNodes);
    std::ostringstream dump;
    cosmos::print_ledger(dump, *injector);
    assert(dump.str().find("SKIPPED") != std::string::npos);
    assert(dump.str().find("reason=max_crashed_nodes") != std::string::npos);
    std::cout << "[PASS] test_max_crashed_nodes_refuses" << std::endl;
}

// 4. min_healthy_quorum refuses a crash that would leave fewer than the promised live nodes.
void test_min_healthy_quorum_refuses() {
    FaultConfig cfg = base_config();
    cfg.max_crashed_nodes = 2;
    cfg.min_healthy_quorum = 1;
    Simulator sim(kSeed);
    assert(sim.install_faults(cfg, kNodes).has_value());
    auto* injector = sim.injector_or_null();

    assert(injector->crash_node(0, ms(5)).has_value());
    assert(injector->crash_node(1, ms(5)).has_value());
    assert(!injector->crash_node(2, ms(5)).has_value());
    assert(last_row(sim).reason == SkipReason::MinHealthyQuorum);
    std::cout << "[PASS] test_min_healthy_quorum_refuses" << std::endl;
}

// 5. Unknown and already-crashed nodes are refused rather than silently accepted.
void test_unknown_and_double_crash_refused() {
    Simulator sim(kSeed);
    assert(sim.install_faults(base_config(), kNodes).has_value());
    auto* injector = sim.injector_or_null();

    assert(!injector->crash_node(9, ms(1)).has_value());
    assert(last_row(sim).reason == SkipReason::UnknownNode);
    assert(injector->crash_node(0, ms(5)).has_value());
    assert(!injector->crash_node(0, ms(5)).has_value());
    assert(last_row(sim).reason == SkipReason::AlreadyCrashed);
    const size_t active = injector->active_episodes();
    injector->heal(999); // unknown id: a no-op, not UB
    assert(injector->active_episodes() == active);
    std::cout << "[PASS] test_unknown_and_double_crash_refused" << std::endl;
}

// 6. Quiesce heals every active episode and refuses new starts afterwards.
void test_begin_quiesce_heals_and_blocks() {
    Simulator sim(kSeed);
    assert(sim.install_faults(base_config(), kNodes).has_value());
    auto* injector = sim.injector_or_null();

    assert(injector->crash_node(1, ms(50)).has_value());
    injector->begin_quiesce();
    assert(injector->quiescing());
    assert(sim.nodes().is_live(1));
    assert(injector->active_episodes() == 0);
    assert(!injector->crash_node(2, ms(1)).has_value());
    assert(last_row(sim).reason == SkipReason::Quiesce);
    sim.scheduler().run_until_quiescence();
    assert(sim.now().ns == 0);
    std::cout << "[PASS] test_begin_quiesce_heals_and_blocks" << std::endl;
}

// 7. A crash episode discards un-synced storage at heal and keeps the synced bytes.
void test_crash_episode_discards_un_synced_storage() {
    char dir[] = "/tmp/cosmos_episode_XXXXXX";
    assert(mkdtemp(dir) != nullptr);
    char path[128];
    snprintf(path, sizeof(path), "%s/node1.log", dir);

    Simulator sim(kSeed);
    assert(sim.install_faults(base_config(), kNodes).has_value());
    Simulator::set_current(&sim);

    struct Writer {
        const char* path = nullptr;
    };
    Writer writer{path};
    auto write_file = +[](void* context) -> void* {
        Writer* w = static_cast<Writer*>(context);
        const int fd = open(w->path, O_CREAT | O_RDWR | O_TRUNC, 0600);
        assert(fd >= 3);
        assert(write(fd, "sync", 4) == 4);
        assert(fsync(fd) == 0);
        assert(write(fd, "lost", 4) == 4); // never synced
        close(fd);
        return nullptr;
    };
    pthread_t id = 0;
    assert(sim.spawn_node(1, &id, write_file, &writer) == 0);
    sim.scheduler().run_until_quiescence();

    auto* injector = sim.injector_or_null();
    assert(injector->crash_node(1, ms(2)).has_value());
    sim.scheduler().run_until_quiescence();
    assert(sim.nodes().is_live(1));
    Simulator::set_current(nullptr);

    const int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    char buffer[16] = {};
    assert(pread(fd, buffer, sizeof(buffer), 0) == 4);
    assert(memcmp(buffer, "sync", 4) == 0);
    close(fd);
    unlink(path);
    rmdir(dir);
    std::cout << "[PASS] test_crash_episode_discards_un_synced_storage" << std::endl;
}

// 8. A partition drops cross-group traffic and stops dropping when its heal fires.
struct NetFixture {
    int client_send_returns[2] = {-1, -1};
    char server_got[8] = {};
    int server_eagain = 0;
    int refused_connect = 0;
    int refused_errno = 0;
};

sockaddr_in loopback(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

void* server_node(void* context) {
    NetFixture* fixture = static_cast<NetFixture*>(context);
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    const sockaddr_in addr = loopback(7101);
    assert(bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(listen(listener, 1) == 0);
    const int fd = accept(listener, nullptr, nullptr);
    assert(fd >= 0);
    timespec wait{};
    wait.tv_nsec = 6'000'000L;
    nanosleep(&wait, nullptr);
    assert(recv(fd, fixture->server_got, sizeof(fixture->server_got), 0) == 4);
    assert(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    fixture->server_eagain = recv(fd, fixture->server_got, sizeof(fixture->server_got), 0);
    return nullptr;
}

void* client_node(void* context) {
    NetFixture* fixture = static_cast<NetFixture*>(context);
    timespec wait{};
    wait.tv_nsec = 1'000'000L;
    nanosleep(&wait, nullptr);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    const sockaddr_in addr = loopback(7101);
    assert(connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    wait.tv_nsec = 2'000'000L;
    nanosleep(&wait, nullptr); // t=3ms: partition is active
    const int probe = socket(AF_INET, SOCK_STREAM, 0);
    assert(probe >= 0);
    fixture->refused_connect =
        static_cast<int>(connect(probe, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)));
    fixture->refused_errno = errno;
    close(probe);
    fixture->client_send_returns[0] = static_cast<int>(send(fd, "ping", 4, 0));
    wait.tv_nsec = 3'000'000L;
    nanosleep(&wait, nullptr); // t=6ms: healed
    fixture->client_send_returns[1] = static_cast<int>(send(fd, "ping", 4, 0));
    return nullptr;
}

void arm_partition(void* context) {
    auto* injector = static_cast<cosmos::BasicFaultInjector<cosmos::VirtualClock>*>(context);
    assert(injector->start_partition({1}, {2}, ms(3)).has_value());
}

void test_partition_drops_until_healed() {
    Simulator sim(kSeed);
    assert(sim.install_faults(base_config(), kNodes).has_value());
    Simulator::set_current(&sim);
    auto* injector = sim.injector_or_null();

    NetFixture fixture;
    pthread_t server = 0;
    pthread_t client = 0;
    assert(sim.spawn_node(1, &server, server_node, &fixture) == 0);
    assert(sim.spawn_node(2, &client, client_node, &fixture) == 0);
    assert(sim.at(at_ms(2), arm_partition, injector));

    sim.scheduler().run_until_quiescence();
    assert(fixture.client_send_returns[0] == 4); // dropped, send still succeeded
    assert(fixture.client_send_returns[1] == 4); // delivered after the heal
    assert(memcmp(fixture.server_got, "ping", 4) == 0);
    assert(fixture.server_eagain == -1);
    assert(fixture.refused_connect == -1);
    assert(fixture.refused_errno == ECONNREFUSED);
    assert(sim.net().partition_count() == 0);
    assert(injector->active_episodes() == 0);
    assert(injector->ledger().size() == 2);
    Simulator::set_current(nullptr);
    std::cout << "[PASS] test_partition_drops_until_healed" << std::endl;
}

// 9. A withheld episode start in replay is a recorded suppression, not a partition.
void test_episode_start_is_suppressible_in_replay() {
    Simulator recorded(kSeed);
    assert(recorded.install_faults(base_config(), kNodes, TraceOptions{true, 32}).has_value());
    auto* first = recorded.injector_or_null();
    const auto id = first->start_partition({1}, {2}, ms(1));
    assert(id.has_value());
    recorded.scheduler().run_until_quiescence();
    const FaultTrace source = first->trace();
    assert(source.size() == 1);
    assert(source[0].site == SiteId::partition);

    Simulator replayed(kSeed);
    assert(replayed.install_faults(base_config(), kNodes, TraceOptions{true, 32}).has_value());
    auto* second = replayed.injector_or_null();
    second->replay_from(source, TraceFilter::withhold({source[0].seq}));
    assert(!second->start_partition({1}, {2}, ms(1)).has_value());
    assert(replayed.net().partition_count() == 0);
    assert(last_row(replayed).status == DecisionStatus::Suppressed);

    Simulator honoured(kSeed);
    assert(honoured.install_faults(base_config(), kNodes, TraceOptions{true, 32}).has_value());
    auto* third = honoured.injector_or_null();
    third->replay_from(source, TraceFilter::honour_all());
    assert(third->start_partition({1}, {2}, ms(1)).has_value());
    assert(honoured.net().partition_count() == 1);
    std::cout << "[PASS] test_episode_start_is_suppressible_in_replay" << std::endl;
}

// 10. Limits hold under repeated starts; the ledger is the same for the same seed and script.
void test_limits_hold_and_the_ledger_is_stable() {
    std::string dumps[2];
    for (int run = 0; run < 2; ++run) {
        FaultConfig cfg = base_config();
        cfg.max_crashed_nodes = 1;
        Simulator sim(kSeed);
        assert(sim.install_faults(cfg, kNodes).has_value());
        auto* injector = sim.injector_or_null();
        for (int step = 0; step < 50; ++step) {
            (void)injector->crash_node(static_cast<cosmos::NodeId>(step % kNodes), ms(1));
            assert(sim.nodes().crashed_count() <= 1u);
            injector->heal_all();
            assert(sim.nodes().crashed_count() == 0);
        }
        // One left active at quiesce: the terminal heal must clear it, not the test.
        assert(injector->crash_node(0, ms(5)).has_value());
        injector->begin_quiesce();
        assert(injector->active_episodes() == 0);
        assert(sim.nodes().live_count() == kNodes);
        std::ostringstream dump;
        cosmos::print_ledger(dump, *injector);
        dumps[run] = dump.str();
    }
    assert(dumps[0] == dumps[1]);
    std::cout << "[PASS] test_limits_hold_and_the_ledger_is_stable" << std::endl;
}

// 11. A crash wakes the crashed node's blocked calls and shows the peer an EOF.
struct CrashNet {
    int client_recv = 99;
    int client_errno = 0;
    int server_recv = 99;
};

void* crash_client(void* context) {
    CrashNet* fixture = static_cast<CrashNet*>(context);
    timespec wait{};
    wait.tv_nsec = 1'000'000L;
    nanosleep(&wait, nullptr);
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    const sockaddr_in addr = loopback(7102);
    assert(connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    char buffer[4] = {};
    fixture->client_recv = static_cast<int>(recv(fd, buffer, sizeof(buffer), 0));
    fixture->client_errno = errno;
    return nullptr;
}

void* crash_server(void* context) {
    CrashNet* fixture = static_cast<CrashNet*>(context);
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    const sockaddr_in addr = loopback(7102);
    assert(bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(listen(listener, 1) == 0);
    const int fd = accept(listener, nullptr, nullptr);
    assert(fd >= 0);
    timespec wait{};
    wait.tv_nsec = 3'000'000L;
    nanosleep(&wait, nullptr);
    assert(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
    char buffer[4] = {};
    fixture->server_recv = static_cast<int>(recv(fd, buffer, sizeof(buffer), 0));
    return nullptr;
}

void crash_node_two(void* context) {
    auto* injector = static_cast<cosmos::BasicFaultInjector<cosmos::VirtualClock>*>(context);
    assert(injector->crash_node(2, ms(5)).has_value());
}

void test_crash_wakes_blocked_calls() {
    Simulator sim(kSeed);
    assert(sim.install_faults(base_config(), kNodes).has_value());
    Simulator::set_current(&sim);
    auto* injector = sim.injector_or_null();

    CrashNet fixture;
    pthread_t server = 0;
    pthread_t client = 0;
    assert(sim.spawn_node(1, &server, crash_server, &fixture) == 0);
    assert(sim.spawn_node(2, &client, crash_client, &fixture) == 0);
    assert(sim.at(at_ms(2), crash_node_two, injector));

    sim.scheduler().run_until_quiescence();
    assert(fixture.client_recv == -1);
    assert(fixture.client_errno == EBADF);
    assert(fixture.server_recv == 0); // peer closed
    assert(sim.nodes().is_live(2));   // the scheduled heal rebooted it
    Simulator::set_current(nullptr);
    std::cout << "[PASS] test_crash_wakes_blocked_calls" << std::endl;
}

// 12. A scheduled crash fires at exactly its at time, draws nothing, and heals itself.
void test_scheduled_crash_fires_at_its_time() {
    FaultConfig cfg = base_config();
    cfg.scheduled_episodes.push_back({at_ms(2), cosmos::CrashNode{1}, ms(2)});

    Simulator sim(kSeed);
    assert(sim.install_faults(cfg, kNodes, TraceOptions{true, 16}).has_value());
    auto* injector = sim.injector_or_null();
    sim.scheduler().run_until_quiescence();

    assert(sim.nodes().is_live(1));
    assert(injector->active_episodes() == 0);
    assert(injector->draws() == 0);
    assert(injector->ledger().size() == 2);
    assert(injector->ledger()[0].at.ns == 2'000'000);
    assert(injector->ledger()[0].site == SiteId::crash_node);
    assert(!injector->ledger()[0].drew);
    assert(injector->ledger()[1].at.ns == 4'000'000);
    assert(injector->trace().size() == 1);
    assert(injector->trace()[0].site == SiteId::crash_node);
    assert(injector->trace()[0].amount.ns == 2'000'000);
    assert(!injector->trace()[0].drew);
    std::cout << "[PASS] test_scheduled_crash_fires_at_its_time" << std::endl;
}

// 13. A scheduled start that would break a limit is refused and recorded, not silently dropped.
void test_scheduled_start_is_refused_by_limits() {
    FaultConfig cfg = base_config();
    cfg.max_crashed_nodes = 1;
    cfg.scheduled_episodes.push_back({at_ms(1), cosmos::CrashNode{0}, ms(5)});
    cfg.scheduled_episodes.push_back({at_ms(2), cosmos::CrashNode{1}, ms(5)});

    Simulator sim(kSeed);
    assert(sim.install_faults(cfg, kNodes).has_value());
    auto* injector = sim.injector_or_null();
    sim.scheduler().run_until_quiescence();

    assert(injector->ledger().size() == 3); // fired 1ms, refused 2ms, healed 6ms
    assert(injector->ledger()[0].status == DecisionStatus::Fired);
    assert(injector->ledger()[1].status == DecisionStatus::Skipped);
    assert(injector->ledger()[1].reason == SkipReason::MaxCrashedNodes);
    assert(injector->ledger()[2].at.ns == 6'000'000);
    std::cout << "[PASS] test_scheduled_start_is_refused_by_limits" << std::endl;
}

// 14. A scheduled partition splits for its window and heals on time.
void test_scheduled_partition_heals() {
    FaultConfig cfg = base_config();
    cfg.scheduled_episodes.push_back({at_ms(1), cosmos::Partition{{1}, {2}}, ms(2)});

    Simulator sim(kSeed);
    assert(sim.install_faults(cfg, kNodes).has_value());
    auto* injector = sim.injector_or_null();
    sim.scheduler().run_until_quiescence();

    assert(sim.net().partition_count() == 0);
    assert(injector->ledger().size() == 2);
    assert(injector->ledger()[0].at.ns == 1'000'000);
    assert(injector->ledger()[1].at.ns == 3'000'000);
    assert(injector->active_episodes() == 0);
    std::cout << "[PASS] test_scheduled_partition_heals" << std::endl;
}

int g_ticks[2][8];
int g_tick_count[2];

struct Ticker {
    int node = 0;
};

void* ticker(void* context) {
    Ticker* ticker_state = static_cast<Ticker*>(context);
    for (int i = 0; i < 3; ++i) {
        timespec wait{};
        wait.tv_nsec = 1'000'000L;
        nanosleep(&wait, nullptr);
        const int slot = ticker_state->node - 1;
        if (g_tick_count[slot] < 8) {
            g_ticks[slot][g_tick_count[slot]] =
                static_cast<int>(Simulator::current()->now().ns / 1'000'000);
            ++g_tick_count[slot];
        }
    }
    return nullptr;
}

// 15. A paused node's fibers stop at their next wake and resume only when the pause heals.
void test_pause_stops_and_resumes_a_node() {
    for (auto& row : g_ticks) {
        for (int& value : row)
            value = -1;
    }
    g_tick_count[0] = 0;
    g_tick_count[1] = 0;

    FaultConfig cfg = base_config();
    cfg.scheduled_episodes.push_back({at_ms(2), cosmos::PauseNode{1}, ms(2)});

    Simulator sim(kSeed);
    assert(sim.install_faults(cfg, kNodes).has_value());
    Simulator::set_current(&sim);
    auto* injector = sim.injector_or_null();
    Ticker one{1};
    Ticker two{2};
    pthread_t first = 0;
    pthread_t second = 0;
    assert(sim.spawn_node(1, &first, ticker, &one) == 0);
    assert(sim.spawn_node(2, &second, ticker, &two) == 0);
    sim.scheduler().run_until_quiescence();

    assert(g_tick_count[0] == 3);
    assert(g_ticks[0][0] == 1);
    assert(g_ticks[0][1] == 4); // 2ms and 3ms were spent paused
    assert(g_ticks[0][2] == 5);
    assert(g_tick_count[1] == 3);
    assert(g_ticks[1][0] == 1);
    assert(g_ticks[1][1] == 2);
    assert(g_ticks[1][2] == 3);
    assert(!sim.scheduler().node_paused(1));
    assert(injector->ledger().size() == 2);
    assert(injector->ledger()[0].site == SiteId::pause_node);
    assert(injector->ledger()[1].at.ns == 4'000'000);
    Simulator::set_current(nullptr);
    std::cout << "[PASS] test_pause_stops_and_resumes_a_node" << std::endl;
}

// 16. One node-fault at a time; a pause is not charged to the crash budget.
void test_node_faults_do_not_overlap() {
    Simulator sim(kSeed);
    assert(sim.install_faults(base_config(), kNodes).has_value());
    auto* injector = sim.injector_or_null();

    const auto paused = injector->pause_node(1, ms(5));
    assert(paused.has_value());
    assert(sim.nodes().crashed_count() == 0);
    assert(injector->crash_node(1, ms(5)) == std::nullopt);
    assert(last_row(sim).reason == SkipReason::NodeBusy);
    assert(injector->pause_node(1, ms(5)) == std::nullopt);
    assert(last_row(sim).reason == SkipReason::NodeBusy);
    assert(injector->start_partition({}, {2}, ms(1)) == std::nullopt);
    assert(last_row(sim).reason == SkipReason::BadPartition);

    injector->heal(*paused);
    assert(!sim.scheduler().node_paused(1));
    assert(injector->crash_node(1, ms(1)).has_value());
    std::cout << "[PASS] test_node_faults_do_not_overlap" << std::endl;
}

// 22. A half-armed install withdraws what it scheduled; losing the injector must not leave a
// callback pointing into it.
void test_arm_failure_withdraws_armed_events() {
    Simulator sim(kSeed);
    int fired = 0;
    auto tick = +[](void* context) { ++*static_cast<int*>(context); };
    const int fill = static_cast<int>(cosmos::Scheduler::kMaxDeadlines) - 1;
    for (int i = 0; i < fill; ++i) {
        assert(sim.at(Time{1'000'000}, tick, &fired));
    }

    FaultConfig cfg = base_config();
    cfg.scheduled_episodes.push_back({at_ms(1), cosmos::CrashNode{0}, ms(1)});
    cfg.scheduled_episodes.push_back({at_ms(1), cosmos::CrashNode{1}, ms(1)});
    const auto installed = sim.install_faults(cfg, kNodes);
    assert(!installed.has_value());
    assert(installed.error().error == cosmos::ConfigError::TooManyScheduledEpisodes);
    assert(sim.injector_or_null() == nullptr);
    assert(sim.scheduler().pending_deadlines() == static_cast<size_t>(fill));
    sim.scheduler().run_until_quiescence();
    assert(fired == fill);
    std::cout << "[PASS] test_arm_failure_withdraws_armed_events" << std::endl;
}

// 17. The liveness transition is one-way.
void test_enter_liveness_mode_is_one_way() {
    Simulator sim(kSeed);
    assert(sim.install_faults(base_config(), kNodes).has_value());
    auto* injector = sim.injector_or_null();
    assert(injector->enter_liveness_mode({0, 1}));
    assert(!injector->enter_liveness_mode({0, 1}));
    assert(injector->liveness());
    std::cout << "[PASS] test_enter_liveness_mode_is_one_way" << std::endl;
}

// 18. Core faults heal at the transition; non-core faults become persistent and survive time.
void test_liveness_heals_core_and_persists_non_core() {
    FaultConfig cfg = base_config();
    cfg.max_crashed_nodes = 2;
    cfg.min_healthy_quorum = 0;

    Simulator sim(kSeed);
    assert(sim.install_faults(cfg, kNodes).has_value());
    auto* injector = sim.injector_or_null();
    assert(injector->crash_node(0, ms(10)).has_value());
    assert(injector->crash_node(2, ms(10)).has_value());
    assert(sim.nodes().crashed_count() == 2);

    assert(injector->enter_liveness_mode({0, 1}));
    assert(sim.nodes().is_live(0));    // core fault healed
    assert(sim.nodes().is_crashed(2)); // non-core fault kept
    assert(injector->active_episodes() == 1);

    sim.scheduler().run_until_quiescence();
    assert(sim.now().ns == 0); // its heal deadline was withdrawn
    assert(sim.nodes().is_crashed(2));

    injector->begin_quiesce();
    assert(sim.nodes().is_live(2));
    assert(injector->active_episodes() == 0);
    std::cout << "[PASS] test_liveness_heals_core_and_persists_non_core" << std::endl;
}

// 19. After the transition, a fault on no core node is persistent by construction.
void test_non_core_faults_after_transition_persist() {
    Simulator sim(kSeed);
    assert(sim.install_faults(base_config(), kNodes).has_value());
    auto* injector = sim.injector_or_null();
    assert(injector->enter_liveness_mode({0}));

    assert(injector->crash_node(2, ms(1)).has_value());
    assert(injector->abandon_node(1).has_value());
    sim.scheduler().run_until_quiescence();
    assert(sim.nodes().is_crashed(2));
    assert(sim.nodes().is_crashed(1));
    assert(injector->active_episodes() == 2);

    std::ostringstream dump;
    cosmos::print_ledger(dump, *injector);
    assert(dump.str().find("persistent") != std::string::npos);

    assert(injector->start_partition({1}, {2}, ms(1)).has_value());
    sim.scheduler().run_until_quiescence();
    assert(sim.net().partition_count() == 1); // persistent too

    injector->begin_quiesce();
    assert(sim.nodes().live_count() == kNodes);
    assert(sim.net().partition_count() == 0);
    std::cout << "[PASS] test_non_core_faults_after_transition_persist" << std::endl;
}

// 20. Core faults still heal normally after the transition.
void test_core_faults_after_transition_still_heal() {
    Simulator sim(kSeed);
    assert(sim.install_faults(base_config(), kNodes).has_value());
    auto* injector = sim.injector_or_null();
    assert(injector->enter_liveness_mode({0}));

    assert(injector->crash_node(0, ms(2)).has_value());
    sim.scheduler().run_until_quiescence();
    assert(sim.nodes().is_live(0));
    assert(injector->ledger().size() == 2);
    assert(!injector->ledger()[0].persistent);
    std::cout << "[PASS] test_core_faults_after_transition_still_heal" << std::endl;
}

// 21. Harness-side sampling of `at` is deterministic and still zero-draw at runtime.
void test_sampled_at_times_are_deterministic() {
    int64_t fired_at[2] = {};
    int64_t planned_at[2] = {};
    for (int run = 0; run < 2; ++run) {
        Simulator sim(kSeed);
        planned_at[run] = static_cast<int64_t>(sim.swarm_rng().next() % 5'000'000) + 1'000'000;
        FaultConfig cfg = base_config();
        cfg.scheduled_episodes.push_back({Time{planned_at[run]}, cosmos::CrashNode{1}, ms(1)});
        assert(sim.install_faults(cfg, kNodes).has_value());
        sim.scheduler().run_until_quiescence();
        auto* injector = sim.injector_or_null();
        assert(injector->draws() == 0);
        fired_at[run] = injector->ledger()[0].at.ns;
        assert(fired_at[run] == planned_at[run]);
    }
    assert(planned_at[0] == planned_at[1]);
    std::cout << "[PASS] test_sampled_at_times_are_deterministic" << std::endl;
}

} // namespace

int main() {
    test_crash_episode_heals_automatically();
    test_heal_cancels_the_auto_heal();
    test_max_crashed_nodes_refuses();
    test_min_healthy_quorum_refuses();
    test_unknown_and_double_crash_refused();
    test_begin_quiesce_heals_and_blocks();
    test_crash_episode_discards_un_synced_storage();
    test_partition_drops_until_healed();
    test_episode_start_is_suppressible_in_replay();
    test_limits_hold_and_the_ledger_is_stable();
    test_crash_wakes_blocked_calls();
    test_scheduled_crash_fires_at_its_time();
    test_scheduled_start_is_refused_by_limits();
    test_scheduled_partition_heals();
    test_pause_stops_and_resumes_a_node();
    test_node_faults_do_not_overlap();
    test_arm_failure_withdraws_armed_events();
    test_enter_liveness_mode_is_one_way();
    test_liveness_heals_core_and_persists_non_core();
    test_non_core_faults_after_transition_persist();
    test_core_faults_after_transition_still_heal();
    test_sampled_at_times_are_deterministic();
    std::cout << "All episode tests passed successfully!" << std::endl;
    return 0;
}
