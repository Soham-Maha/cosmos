# Cosmos library — design & public API reference

This is the authoritative design document for `libcosmos`. It defines the
architecture, every public interface, and the semantics that make simulation
runs deterministic and explorable. If implementation and this document
disagree, fix one of them — they must stay in sync.

Audience: (a) us, building the library; (b) application authors embedding it.

---

## 1. Concepts

| Term | Meaning |
|---|---|
| **Universe** | One `Simulator` instance running one seeded execution. |
| **Seed** | 64-bit value; fully determines a universe (same binary ⇒ identical execution). |
| **RNG stream** | Independent deterministic PRNG derived from the seed by domain: `schedule`, `fault`, `workload`, `user`. Changing one domain's consumption never perturbs the others. |
| **Task** | A C++20 coroutine (`cosmos::Task`) cooperatively scheduled by the universe. The unit of concurrency. |
| **Node** | A simulated machine: an ownership group for tasks + endpoints. Can crash and reboot. |
| **Virtual time** | `Time` (int64 ns since universe start). Advances only when nothing is runnable, to the next event. |
| **Event** | A (virtual-time, seq) scheduled occurrence: timer wakeup, packet delivery, scheduled action. |
| **Choice point** | Any place the scheduler could do more than one thing (pick a ready task, deliver vs. delay, fault or not). Resolved by an RNG-stream draw. |
| **Finding** | A violated `always` assertion (or crash) in some universe, or a `sometimes` assertion hit in **no** universe of a campaign. |
| **Campaign** | N universes over N seeds, run in parallel, aggregating findings. |

**Why determinism holds:** the only sources of variation inside a universe are
(1) scheduler choices, (2) time advancement, (3) RNG draws, (4) simulated I/O
outcomes — and all four are pure functions of the seed streams. Application
code that honors the [determinism contract](#9-the-determinism-contract)
introduces no fifth source.

---

## 2. Module map (public headers)

```
include/cosmos/
├── cosmos.hpp      umbrella include
├── time.hpp        Time, Duration, chrono literals
├── random.hpp      Rng: xoshiro256**, stream splitting, coin/range/…
├── task.hpp        Task coroutine type + awaitables (Yield, Sleep)
├── simulator.hpp   Simulator, SimConfig, SimResult  ← the universe
├── net.hpp         Net, Node, Endpoint, Address, Packet, Verdict
├── faults.hpp      FaultProfile, fault scheduling helpers
├── gen.hpp         deterministic data generators (property-testing style)
├── assert.hpp      always / sometimes / reachable / COSMOS_CHECK, reports
├── campaign.hpp    Campaign, CampaignConfig, CampaignReport
└── storage.hpp     (Phase 4) simulated disk, fsync/crash semantics
```

Namespace: `cosmos::`. Chrono literals in `cosmos::literals` (`1ms`, `10s`).

---

## 3. `random.hpp` — deterministic randomness

```cpp
class Rng {
public:
    explicit Rng(uint64_t seed);
    static Rng derive(const Rng& parent, uint64_t domain); // stream splitting

    uint64_t next();                       // xoshiro256** raw
    uint64_t range(uint64_t lo, uint64_t hi);   // inclusive
    bool     coin(double p);               // Bernoulli
    double   uniform();                    // [0,1)
    template <typename C> const typename C::value_type& pick(const C& xs);
};
```

Stream domains (fixed enum, part of the ABI): `Schedule=1, Fault=2, Workload=3,
User=4`. Derivation: seed mixing via splitmix64 (deterministic, order-free).

**Rules:** library internals only ever draw from `schedule`/`fault`. User
workloads draw from `workload` (via `sim.workload()`) or their own `user`
stream (`sim.user_rng()`). This orthogonality is what lets exploration
dimensions vary independently across seeds.

---

## 4. `time.hpp`

```cpp
using Duration = std::chrono::nanoseconds;          // int64, virtual
using Time     = std::chrono::time_point<std::chrono::steady_clock, Duration>;
// (steady_clock used only as a tag type; never reads the OS clock.)

namespace cosmos::literals {
constexpr Duration operator""ms(unsigned long long);
constexpr Duration operator""s (unsigned long long);
}
```

---

## 5. `task.hpp` — cooperative coroutine tasks

Users write plain synchronous-looking code; suspension only at `co_await`
points, which is exactly where the scheduler may interleave.

```cpp
struct Task {
    struct promise_type;
    // move-only; handle owned by the spawning Node
};

// Awaitables (obtain from Simulator; see §7):
co_await sim.yield();          // reschedule (lets others run)
co_await sim.sleep(10ms);      // wake at virtual now+10ms
```

Promise semantics:
- `initial_suspend = suspend_always` — `spawn()` never runs the body inline;
  the task merely becomes *ready*. (Spawn order never leaks caller timing.)
- `final_suspend = suspend_always` — the scheduler reaps finished tasks.
- `unhandled_exception` — captured into `SimResult.failures` (universe ends
  as a finding, with repro seed).
- Each promise carries `Simulator*` (set at spawn) so awaitables can reach
  the universe without globals.

**Cancellation:** node crash destroys the node's task handles. Awaitables must
be trivially destructible; user RAII in coroutine frames runs normally on
destruction.

---

## 6. `simulator.hpp` — the universe

```cpp
struct SimConfig {
    uint64_t seed = 0;
    Duration time_limit = 300s;      // universe ends (ok) when exceeded
    uint64_t event_limit = 10'000'000; // livelock guard
    std::ostream* trace = nullptr;   // optional structured trace sink
};

struct Failure {
    std::string kind;     // "assert_always" | "exception" | "event_limit"
    std::string id;       // assertion id / what
    std::string detail;   // message, file:line
    Time at;
};
struct SimResult {
    bool ok() const { return failures.empty(); }
    std::vector<Failure> failures;
    uint64_t trace_hash = 0;   // FNV-1a over the event trace (if tracing)
    Time final_time;
    uint64_t events_processed;
};

class Simulator {
public:
    explicit Simulator(SimConfig);

    // --- lifecycle ---
    SimResult run();                 // run to quiescence / time_limit
    // --- tasks ---
    Node& create_node(std::string name);
    template <typename F> void spawn(Node&, F&& coro_fn); // F: () -> Task
    void crash(Node&);               // kill tasks + close endpoints
    void reboot(Node&);              // mark alive; user re-spawns logic
    // --- time ---
    Time now() const;
    SleepAwaitable sleep(Duration);
    YieldAwaitable yield();
    template <typename F> void at(Time, F&&);        // one-shot action
    template <typename F> void every(Duration, F&&); // repeating action
    // --- randomness ---
    Rng& workload_rng();             // 'workload' stream
    Rng& user_rng();                 // 'user' stream
    // --- subsystems ---
    Net& net();
    // Storage& storage();  // Phase 4
    // --- faults ---
    void set_faults(FaultProfile);
    // --- assertions (also free functions, §10) ---
    void report_always(bool ok, std::string id, std::string detail);
    void report_sometimes(bool hit, std::string id);
};
```

**Scheduler algorithm (the deterministic core):**

```
loop:
  1. if ready tasks non-empty:
         i = schedule_rng.range(0, ready.size()-1)   // THE choice point
         resume ready[i] until it suspends
         continue
  2. if event queue empty: stop (quiescence)
  3. advance virtual clock to earliest event time
     fire all events at that time (in seq order) → may enqueue ready tasks
```

All interleaving exploration concentrates in step 1's draw; all time/fault
exploration in the seeded latencies/faults of step 3's events. One universe =
one path through that choice space.

---

## 7. `net.hpp` — simulated network

```cpp
struct Address { uint32_t node; uint16_t port; };
using Payload  = std::vector<std::byte>;

class Endpoint {                        // bound to (node, port)
public:
    Address addr() const;
    void send(Address to, Payload);                 // fire-and-forget
    RecvAwaitable recv();                           // co_await → (from, Payload)
};

class Net {
public:
    Endpoint& bind(Node&, uint16_t port);

    // faults — declarative profile is set via Simulator::set_faults;
    // imperative control:
    void partition(std::vector<NodeId> a, std::vector<NodeId> b); // cut a<->b
    void heal_all();

    // custom hook — full control, highest precedence:
    using Verdict = std::variant<Drop, DeliverAfter>;
    std::function<Verdict(const PacketView&)> on_send; // nullptr = use profile
};
```

Delivery semantics:
1. `send()` consults, in order: connectivity map (partitions) → `on_send`
   hook → `FaultProfile` (loss/reorder/latency draws from the `fault` stream).
2. Surviving packets become **delivery events** at `now + sampled_latency`.
3. On delivery: if a task is blocked in `recv()`, it becomes ready with the
   packet; otherwise the packet queues on the endpoint (bounded queue →
   backpressure via send suspension is a Phase-2 refinement).
4. Reorder is modeled by randomized per-packet latency (deterministic, via
   fault stream) — not by explicit permutation.

Node crash closes its endpoints: pending packets to it are dropped; blocked
`recv()`s are cancelled with their tasks.

---

## 8. `faults.hpp` — how users define faults

```cpp
struct FaultProfile {
    double     packet_loss   = 0.0;        // Bernoulli per packet
    double     reorder_rate  = 0.0;        // extra latency jitter probability
    LatencyGen latency       = constant(1ms);   // base delivery latency
    CrashGen   crash_interval{};           // optional random node crashes
    // Phase 4: disk_error_rate, torn_write_on_crash, ...
};
```

Four ways to inject faults (compose freely):

| Mechanism | API | Use for |
|---|---|---|
| **Declarative** | `sim.set_faults(FaultProfile)` | background hostility: loss, latency, random crashes — sampled from the `fault` stream |
| **Imperative** | `net().partition(...)`, `net().heal_all()`, `sim.crash(node)`, `sim.reboot(node)` | scenario-specific events |
| **Scheduled** | `sim.at(t, fn)` / `sim.every(d, fn)` wrapping the imperative calls | deterministic fault timelines ("partition at t=10s, heal at t=15s") |
| **Custom hook** | `net().on_send = fn → Verdict` | anything computable: byzantine-ish drops, targeted delays, traffic shaping |

Semantics notes:
- Scheduled faults are deterministic (fixed virtual times). Profile/hook
  faults draw from the `fault` stream → reproducible per seed, varied across
  seeds.
- `crash()` destroys the node's tasks/endpoints; `reboot()` marks it alive —
  the app decides what state survives (in-memory state is gone; Phase-4
  simulated disk persists per its fsync model).
- Because faults live in the `fault` stream, a failing run's **entire fault
  timeline replays exactly** from its seed.

---

## 9. `gen.hpp` — how simulation data is generated

Property-testing-style generators over a seeded `Rng` (usually
`sim.workload_rng()`). They are plain functions — compose your own.

```cpp
namespace cosmos::gen {
    uint64_t    range(Rng&, uint64_t lo, uint64_t hi);
    bool        coin(Rng&, double p);
    template<typename T> T one_of(Rng&, std::initializer_list<T>);
    template<typename T> T weighted(Rng&, std::initializer_list<std::pair<double,T>>);
    std::string string(Rng&, size_t len, std::string_view alphabet = alnum);
    std::string string(Rng&, size_t min_len, size_t max_len);
    Duration    exponential(Rng&, Duration mean);      // inter-arrival times
    Duration    lognormal(Rng&, Duration median, Duration p99);
    template<typename F> auto many(Rng&, size_t n, F elem_gen); // vector
}
```

**The workload pattern** (how applications get their simulation data):

```cpp
cosmos::Task client(Simulator& sim, Node& me, Address server) {
    auto& g   = sim.workload_rng();
    auto& ep  = sim.net().bind(me, 0);
    for (;;) {
        co_await sim.sleep(gen::exponential(g, 5ms));   // arrival process
        Op op = gen::weighted(g, {{0.7, Op::Get}, {0.25, Op::Put}, {0.05, Op::Del}});
        Key k = gen::range(g, 0, 999);                  // key space
        ep.send(server, encode(op, k, gen::string(g, 8)));
        // ... co_await reply, assert on it ...
    }
}
```

Three data channels, all reproducible:
1. **Generators** (above) — dynamic inputs: operations, payloads, timings.
2. **Fixtures** — initial state handed to the universe at build time
   (preloaded DB contents, topology descriptors); plain values in the run
   config, i.e. effectively part of the seed.
3. **Swarm config** — the campaign derives per-universe knobs (fault rates,
   key-space size, client counts) from the seed before construction, so
   different universes explore different *regimes*, not just different
   interleavings. (This is FDB's "swarm testing" and it is disproportionately
   effective per CPU-second.)

---

## 10. `assert.hpp` — properties

```cpp
namespace cosmos {
    // evaluated per universe; violation = finding (with seed, time, detail)
    void always(bool cond, std::string id, std::string detail = "");
    // evaluated per campaign: id must be hit in ≥1 universe, else finding
    void sometimes(bool cond, std::string id);
    inline void reachable(std::string id) { sometimes(true, id); }
}
#define COSMOS_CHECK(cond, id) ::cosmos::always((cond), (id), \
        std::string(__FILE__) + ":" + std::to_string(__LINE__))
```

Design rules:
- `always` = safety ("never bad"): serializability violation, divergence,
  crash, lost acknowledged write.
- `sometimes` = liveness/coverage ("eventually good"): leader got elected,
  partition healed, retried request succeeded. Campaign-wide semantics are
  what make liveness testable (Antithesis's key insight — a liveness property
  is falsified only by a *set* of runs).
- Assertions are context-free: they may live in app code, client actors, or
  dedicated **checker actors** (`sim.every(1s, check_cluster_invariants)`).
- Every finding records `(seed, virtual time, id, detail)` → repro is
  `cosmos-example --seed S`.

---

## 11. `campaign.hpp` — state-space exploration

```cpp
struct CampaignConfig {
    uint64_t trials     = 1000;
    uint64_t base_seed  = 0;
    unsigned parallel   = std::thread::hardware_concurrency();
    bool     verify     = false;   // double-run % of trials, compare trace_hash
    SimConfig sim;                 // time_limit, trace sink, etc.
};

struct CampaignReport {
    uint64_t runs, failed_runs;
    std::vector<Failure> findings;            // deduped by (kind,id)
    std::vector<std::string> never_hit;       // sometimes-ids hit 0 times
    uint64_t universes_per_second;
};

class Campaign {
public:
    // build_fn runs in the trial's thread; must construct the whole system
    // inside sim and return. Fully self-contained per universe.
    using BuildFn = std::function<void(Simulator&, uint64_t seed)>; // swarm hook
    static CampaignReport run(CampaignConfig, BuildFn);
};
```

Execution model:
- Trials are sharded across `parallel` worker threads; each trial constructs a
  fresh `Simulator` with seed = `mix(base_seed, trial_index)` and runs it to
  completion. No shared mutable state between universes (except the results
  sink, under mutex).
- **Exploration dimensions per seed:** scheduler interleavings (schedule
  stream), fault timeline (fault stream), workload data (workload stream),
  swarm config (pre-construction draws). One seed ⇒ a fully determined point
  in that product space.
- **Output:** for each finding, a repro command; for each `sometimes` id never
  hit, a coverage warning; aggregate stats.
- **Verify mode:** re-runs a sample (e.g. 1 in 64) of seeds twice and compares
  `trace_hash` — catches contract violations (your "is it still
  deterministic?" regression test).

### Exploration roadmap (later phases)

| Version | Mechanism |
|---|---|
| v1 (MVP) | seeded fuzzing campaign (above) |
| v2 | **coverage guidance**: build the SUT with `-fsanitize-coverage=trace-pc-guard`; edge counts per run feed a seed corpus (keep seeds that find new edges, mutate them — AFL-style, per Antithesis's coverage-guided exploration) |
| v3 | **fork()-branching**: at deep/interesting choice points, `fork()` the universe (COW snapshot), explore the sibling branch in the child — the Antithesis multiverse at process level; **decision-log minimization**: record schedule/fault decisions, delta-debug failing runs to minimal repros; **trace export** (JSON / chrome-trace) for debugging UX |
| v4 (optional) | systematic strategies à la Coyote/dBug (PCT: prioritize few context-switch points) instead of uniform random interleavings |

---

## 12. `storage.hpp` (Phase 4 — defined now so the model is fixed)

```cpp
class Storage {                         // one per Node
public:
    File& open(std::string path);
};
class File {
public:
    WriteAwaitable write(uint64_t offset, std::span<const std::byte>);
    ReadAwaitable  read (uint64_t offset, size_t len);
    FsyncAwaitable fsync();
};
```

Durability model (the part that finds real bugs):
- `write` → page-cache only. `fsync` → durable.
- On `crash` + `reboot`: un-fsynced data is gone; the *last* fsynced region
  may be **torn** (partially applied), decided by the fault stream.
- Latency/errors from `FaultProfile` (`disk_error_rate`, …).
- Deterministic content and timing, seeded per universe.

---

## 13. The determinism contract (user obligations)

Determinism is guaranteed *if* code running inside a universe:

1. Reads time only via `sim.now()` / sleeps via `co_await sim.sleep(...)`.
2. Draws randomness only via `sim.workload_rng()`, `sim.user_rng()`, `gen::*`.
3. Creates no threads; performs no blocking OS I/O; uses only cosmos net/
   storage for I/O. (Pure CPU work is always fine.)
4. Does not let pointer values / addresses influence behavior (no ordering by
   `T*`, no `std::unordered_map<T*>` iteration affecting outcomes — ASLR is a
   nondeterminism source).
5. Avoids `long double`/x87 and `-ffast-math` in behavior-relevant paths.
6. Treats logging as effect-only (logging must not feed back into decisions).

Enforcement aids: verify mode (§11), `COSMOS_STRICT` build that interposes
`clock_gettime`/`getrandom` (later), and code review. The hypervisor stage
(much later) removes this contract entirely — that's precisely its value.

---

## 14. Error handling & debugging

- Exceptions escaping a task ⇒ `Failure{kind:"exception"}`; universe stops;
  campaign reports it with its seed.
- Assertions carry `detail` strings; failures include virtual time.
- Optional trace sink (`SimConfig::trace`): one line per event
  (`t=<ns> deliver n0->n1 seq=…`, `t=<ns> schedule task=…`); `trace_hash`
  (FNV-1a-64) is the universe fingerprint used by verify mode and by users to
  assert sameness in their own CI.
- Repro workflow: campaign prints `--seed S` ⇒ single-run mode re-executes
  exactly; attach gdb (single-threaded ⇒ pleasant); Phase-5 trace export for
  message-sequence visualization.

---

## 15. Non-goals (v1)

- Running unmodified binaries (requires the hypervisor stage).
- Real-time execution / production backends (`real.hpp` — Phase 6).
- Multi-process universes, shared memory, threads inside a universe.
- Byzantine behavior injection beyond user verdict hooks.
- UI/notebook debugging (Antithesis territory; Phase 5 trace export is the
  pragmatic substitute).
