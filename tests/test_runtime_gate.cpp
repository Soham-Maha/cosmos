#include "cosmos/cosmos.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

namespace {

using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::FaultKind;
using cosmos::FaultRule;
using cosmos::Simulator;
using cosmos::SiteId;
using cosmos::Time;

int g_log[16];
int64_t g_stamp[16];
int g_count = 0;

void reset_log() {
    g_count = 0;
    for (int& tag : g_log)
        tag = -1;
}

void log_tag(int tag) {
    const int64_t now = Simulator::current() != nullptr ? Simulator::current()->now().ns : -1;
    if (g_count < 16) {
        g_log[g_count] = tag;
        g_stamp[g_count] = now;
        ++g_count;
    }
}

struct Mark {
    int tag = 0;
    int cancel_after = 0;
    int ready_seen = -1;
};

void mark(void* context) {
    Mark* m = static_cast<Mark*>(context);
    m->ready_seen = static_cast<int>(Simulator::current()->scheduler().ready_count());
    log_tag(m->tag);
    if (m->cancel_after != 0) {
        // Arming then cancelling in the same callback exercises the reclaim path.
        (void)Simulator::current()->cancel_at(static_cast<uint64_t>(m->cancel_after));
    }
}

struct Sleeper {
    int ms = 0;
    int64_t woke_at = -1;
    int tag = 0;
    bool log_node = false;
};

void* sleep_and_record(void* context) {
    Sleeper* s = static_cast<Sleeper*>(context);
    timespec req{};
    req.tv_nsec = static_cast<long>(s->ms) * 1'000'000L;
    nanosleep(&req, nullptr);
    s->woke_at = Simulator::current()->now().ns;
    if (s->log_node) log_tag(static_cast<int>(Simulator::current()->current_node()));
    return nullptr;
}

void* yield_then_log(void* context) {
    Sleeper* s = static_cast<Sleeper*>(context);
    sched_yield();
    log_tag(s->tag);
    s->woke_at = Simulator::current()->now().ns;
    return nullptr;
}

// 1. Timers fire at exactly the requested virtual times, and the clock lands on the latest.
void test_timers_fire_at_exact_virtual_times() {
    Simulator sim(7);
    Simulator::set_current(&sim);
    Sleeper first{5, -1};
    Sleeper second{7, -1};
    pthread_t a = 0;
    pthread_t b = 0;
    assert(sim.spawn_node(0, &a, sleep_and_record, &first) == 0);
    assert(sim.spawn_node(0, &b, sleep_and_record, &second) == 0);
    sim.scheduler().run_until_quiescence();
    assert(first.woke_at == 5'000'000);
    assert(second.woke_at == 7'000'000);
    assert(sim.now().ns == 7'000'000);
    Simulator::set_current(nullptr);
    std::cout << "[PASS] test_timers_fire_at_exact_virtual_times" << std::endl;
}

// 2. Time does not move while any fiber is runnable, only when the ready queue drains.
void test_clock_advances_only_on_quiescence() {
    Simulator sim(11);
    Simulator::set_current(&sim);
    Sleeper yielder{0, -1, 1};
    Sleeper sleeper{2, -1};
    pthread_t a = 0;
    pthread_t b = 0;
    assert(sim.spawn_node(0, &a, yield_then_log, &yielder) == 0);
    assert(sim.spawn_node(0, &b, sleep_and_record, &sleeper) == 0);
    sim.scheduler().run_until_quiescence();
    assert(yielder.woke_at == 0);
    assert(sleeper.woke_at == 2'000'000);
    Simulator::set_current(nullptr);
    std::cout << "[PASS] test_clock_advances_only_on_quiescence" << std::endl;
}

// 3. The same seed reproduces the same interleaving.
void test_same_seed_same_interleaving() {
    reset_log();
    int first_run[3] = {};
    int second_run[3] = {};
    for (int run = 0; run < 2; ++run) {
        Simulator sim(2024);
        Simulator::set_current(&sim);
        Sleeper tasks[3] = {{0, -1, 10}, {0, -1, 11}, {0, -1, 12}};
        pthread_t ids[3] = {};
        for (int i = 0; i < 3; ++i)
            assert(sim.spawn_node(0, &ids[i], yield_then_log, &tasks[i]) == 0);
        sim.scheduler().run_until_quiescence();
        assert(g_count == 3);
        int* out = run == 0 ? first_run : second_run;
        for (int i = 0; i < 3; ++i)
            out[i] = g_log[i];
        Simulator::set_current(nullptr);
        reset_log();
    }
    assert(memcmp(first_run, second_run, sizeof(first_run)) == 0);
    std::cout << "[PASS] test_same_seed_same_interleaving" << std::endl;
}

// 4. Scheduling never consumes the Fault stream, even with an armed memory rule.
void test_scheduling_never_touches_the_fault_stream() {
    reset_log();
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Memory);
    assert(cfg.activate_site(SiteId::malloc));
    FaultRule rule;
    rule.rate = 1.0;
    assert(rule.outcomes.add(FaultKind::OutOfMemory, 1.0));
    assert(cfg.set_rule(SiteId::malloc, rule));

    Simulator sim(99);
    Simulator::set_current(&sim);
    assert(sim.install_faults(cfg, 1).has_value());
    Sleeper tasks[2] = {{0, -1, 1}, {0, -1, 2}};
    pthread_t ids[2] = {};
    for (int i = 0; i < 2; ++i)
        assert(sim.spawn_node(0, &ids[i], yield_then_log, &tasks[i]) == 0);
    sim.scheduler().run_until_quiescence();
    const auto* injector = sim.injector_or_null();
    assert(injector != nullptr);
    assert(injector->draws() == 0);
    assert(injector->injections(SiteId::malloc) == 0);
    Simulator::set_current(nullptr);
    std::cout << "[PASS] test_scheduling_never_touches_the_fault_stream" << std::endl;
}

// 5. Deadlines fire in (at, id) order at exact times; cancel wins even from a callback.
void test_deadlines_fire_on_time_in_order() {
    reset_log();
    Simulator sim(5);
    Simulator::set_current(&sim);
    Mark late{1};
    Mark early{2};
    Mark second_at_two{3};
    Mark first_at_two{4};
    Mark cancelled{5};
    const uint64_t cancel_id = sim.scheduler().schedule_at(Time{1'000'000}, mark, &cancelled);
    assert(cancel_id != 0);
    assert(sim.at(Time{3'000'000}, mark, &late));
    assert(sim.at(Time{1'000'000}, mark, &early));
    assert(sim.at(Time{2'000'000}, mark, &second_at_two));
    assert(sim.at(Time{2'000'000}, mark, &first_at_two));
    assert(sim.cancel_at(cancel_id));

    sim.scheduler().run_until_quiescence();
    assert(g_count == 4);
    assert(g_log[0] == 2);
    assert(g_log[1] == 3);
    assert(g_log[2] == 4);
    assert(g_log[3] == 1);
    assert(g_stamp[0] == 1'000'000);
    assert(g_stamp[1] == 2'000'000);
    assert(g_stamp[2] == 2'000'000);
    assert(g_stamp[3] == 3'000'000);
    Simulator::set_current(nullptr);
    reset_log();
    std::cout << "[PASS] test_deadlines_fire_on_time_in_order" << std::endl;
}

// 6. An event at a timestamp is applied before the fibers woken at that timestamp run.
void test_deadline_fires_before_fiber_at_same_stamp() {
    reset_log();
    Simulator sim(6);
    Simulator::set_current(&sim);
    Mark event{9};
    assert(sim.at(Time{5'000'000}, mark, &event));
    Sleeper sleeper{5, -1, 7};
    pthread_t id = 0;
    assert(sim.spawn_node(0, &id, sleep_and_record, &sleeper) == 0);
    sim.scheduler().run_until_quiescence();
    assert(g_count == 1 && g_log[0] == 9);
    assert(event.ready_seen == 0);
    assert(sleeper.woke_at == 5'000'000);
    Simulator::set_current(nullptr);
    reset_log();
    std::cout << "[PASS] test_deadline_fires_before_fiber_at_same_stamp" << std::endl;
}

// 7. A fiber keeps its node across yields and blocking calls.
void test_node_identity_survives_blocking_calls() {
    reset_log();
    Simulator sim(13);
    Simulator::set_current(&sim);
    Sleeper one{1, -1, 0, true};
    Sleeper three{2, -1, 0, true};
    pthread_t a = 0;
    pthread_t b = 0;
    assert(sim.spawn_node(1, &a, sleep_and_record, &one) == 0);
    assert(sim.spawn_node(3, &b, sleep_and_record, &three) == 0);
    sim.scheduler().run_until_quiescence();
    assert(g_count == 2);
    assert((g_log[0] == 1 || g_log[0] == 3) && g_log[0] != g_log[1]);
    Simulator::set_current(nullptr);
    reset_log();
    std::cout << "[PASS] test_node_identity_survives_blocking_calls" << std::endl;
}

// 8. A file opened by a fiber is owned by that fiber's node: reboot discards its un-synced bytes.
void test_storage_is_attributed_to_the_opening_fiber() {
    char dir[] = "/tmp/cosmos_runtime_XXXXXX";
    assert(mkdtemp(dir) != nullptr);
    char owned[128];
    char other[128];
    snprintf(owned, sizeof(owned), "%s/node2.log", dir);
    snprintf(other, sizeof(other), "%s/node0.log", dir);

    Simulator sim(17);
    Simulator::set_current(&sim);
    sim.nodes().set_node_count(4);

    struct Writer {
        const char* path = nullptr;
        bool sync = false;
    };
    Writer node2{owned, false};
    auto write_from_node = +[](void* context) -> void* {
        Writer* w = static_cast<Writer*>(context);
        const int fd = open(w->path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        assert(fd >= 3);
        assert(write(fd, "node", 4) == 4);
        if (w->sync) assert(fsync(fd) == 0);
        close(fd);
        return nullptr;
    };
    pthread_t id = 0;
    assert(sim.spawn_node(2, &id, write_from_node, &node2) == 0);
    // Node 0 writes a file it fsyncs, so only the node-2 crash may discard anything.
    Writer node0{other, true};
    assert(sim.spawn_node(0, &id, write_from_node, &node0) == 0);
    sim.scheduler().run_until_quiescence();

    assert(sim.crash_node(2));
    assert(sim.reboot_node(2));
    const int un_synced = open(owned, O_RDONLY);
    assert(un_synced >= 0);
    char buffer[8] = {};
    assert(pread(un_synced, buffer, sizeof(buffer), 0) == 0);
    close(un_synced);

    assert(sim.crash_node(0));
    assert(sim.reboot_node(0));
    Simulator::set_current(nullptr);

    const int synced = open(other, O_RDONLY);
    assert(synced >= 0);
    assert(pread(synced, buffer, sizeof(buffer), 0) == 4);
    close(synced);

    unlink(owned);
    unlink(other);
    rmdir(dir);
    std::cout << "[PASS] test_storage_is_attributed_to_the_opening_fiber" << std::endl;
}

// 9b. A write through another node's fd belongs to the writer: reboot(owner) leaves it alone.
void test_write_attribution_follows_the_writer() {
    char dir[] = "/tmp/cosmos_shared_XXXXXX";
    assert(mkdtemp(dir) != nullptr);
    char path[128];
    snprintf(path, sizeof(path), "%s/shared.log", dir);

    Simulator sim(23);
    Simulator::set_current(&sim);
    sim.nodes().set_node_count(3);

    const int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
    assert(fd >= 3);
    assert(write(fd, "AAAA", 4) == 4);
    assert(fsync(fd) == 0);

    struct Shared {
        int fd = -1;
    };
    Shared shared{fd};
    auto write_from_node_one = +[](void* context) -> void* {
        Shared* s = static_cast<Shared*>(context);
        assert(write(s->fd, "BB", 2) == 2); // unsynced, through node 0's fd
        return nullptr;
    };
    pthread_t id = 0;
    assert(sim.spawn_node(1, &id, write_from_node_one, &shared) == 0);
    sim.scheduler().run_until_quiescence();

    assert(sim.crash_node(0));
    assert(sim.reboot_node(0)); // the opener is no longer the writer
    char buffer[8] = {};
    const int probe = openat(AT_FDCWD, path, O_RDONLY);
    assert(probe >= 0);
    assert(pread(probe, buffer, sizeof(buffer), 0) == 6);
    close(probe);

    assert(sim.crash_node(1));
    assert(sim.reboot_node(1)); // the writer's reboot discards its unsynced bytes
    const int after = openat(AT_FDCWD, path, O_RDONLY);
    assert(after >= 0);
    assert(pread(after, buffer, sizeof(buffer), 0) == 4);
    close(after);
    Simulator::set_current(nullptr);
    close(fd);
    unlink(path);
    rmdir(dir);
    std::cout << "[PASS] test_write_attribution_follows_the_writer" << std::endl;
}

// 9. A read-only reopen must not steal a file from the node that owns its un-synced bytes.
void test_reader_does_not_steal_ownership() {
    char dir[] = "/tmp/cosmos_reader_XXXXXX";
    assert(mkdtemp(dir) != nullptr);
    char path[128];
    snprintf(path, sizeof(path), "%s/node2.log", dir);

    Simulator sim(19);
    Simulator::set_current(&sim);
    sim.nodes().set_node_count(3);

    struct Writer {
        const char* path = nullptr;
    };
    Writer writer{path};
    auto write_node = +[](void* context) -> void* {
        Writer* w = static_cast<Writer*>(context);
        const int fd = open(w->path, O_CREAT | O_RDWR | O_TRUNC, 0600);
        assert(fd >= 3);
        assert(write(fd, "AAAA", 4) == 4);
        assert(fsync(fd) == 0);
        assert(write(fd, "BB", 2) == 2); // never synced
        close(fd);
        return nullptr;
    };
    pthread_t id = 0;
    assert(sim.spawn_node(2, &id, write_node, &writer) == 0);
    sim.scheduler().run_until_quiescence();

    const int reader = open(path, O_RDONLY); // node 0 must not become the owner here
    assert(reader >= 3);
    close(reader);

    char buffer[8] = {};
    assert(sim.crash_node(0));
    assert(sim.reboot_node(0));
    int fd = openat(AT_FDCWD, path, O_RDONLY);
    assert(fd >= 0);
    assert(pread(fd, buffer, sizeof(buffer), 0) == 6);
    assert(memcmp(buffer, "AAAABB", 6) == 0);
    close(fd);

    assert(sim.crash_node(2));
    assert(sim.reboot_node(2));
    fd = openat(AT_FDCWD, path, O_RDONLY);
    assert(fd >= 0);
    assert(pread(fd, buffer, sizeof(buffer), 0) == 4);
    assert(memcmp(buffer, "AAAA", 4) == 0);
    close(fd);

    Simulator::set_current(nullptr);
    unlink(path);
    rmdir(dir);
    std::cout << "[PASS] test_reader_does_not_steal_ownership" << std::endl;
}

} // namespace

int main() {
    test_timers_fire_at_exact_virtual_times();
    test_clock_advances_only_on_quiescence();
    test_same_seed_same_interleaving();
    test_scheduling_never_touches_the_fault_stream();
    test_deadlines_fire_on_time_in_order();
    test_deadline_fires_before_fiber_at_same_stamp();
    test_node_identity_survives_blocking_calls();
    test_storage_is_attributed_to_the_opening_fiber();
    test_reader_does_not_steal_ownership();
    test_write_attribution_follows_the_writer();
    std::cout << "All runtime gate tests passed successfully!" << std::endl;
    return 0;
}
