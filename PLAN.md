# Cosmos — Deterministic Simulation Testing as a C++ Library

Cosmos is an embeddable C++ library for **deterministic simulation testing (DST)**.
You link it into your application, write your system against its simulated
environment interfaces (time, scheduling, network, storage, randomness), and
Cosmos can then execute your system in a fully deterministic, fault-injecting,
state-space-exploring simulated world — the same architecture as FoundationDB's
simulator and TigerBeetle's VOPR, packaged as a reusable library.

> **Direction change (2026-07-31):** the original hypervisor-first plan
> (Antithesis-style, Linux+KVM, Rust) is superseded by a **library-first** plan
> in **C++**, due to time constraints. The hypervisor is **not** a separate
> later project: it is a future *execution substrate* (`KvmSubstrate`) that
> implements the same `ISubstrate` seam (Seam B) as the library substrate, so
> it reuses the entire determinism engine. Likewise the production runtime is
> a second *backend* (Seam A) from Phase 1, so one app source builds both a
> testing binary and a normal prod binary. The full layering is in
> `docs/architecture.md`; the Antithesis study notes are preserved in
> `docs/antithesis-study-notes.md`.

---

## Table of Contents

1. [Decisions](#1-decisions)
2. [What "library-first DST" means](#2-what-library-first-dst-means)
3. [Core design (how determinism is achieved)](#3-core-design)
4. [Public API overview](#4-public-api-overview)
5. [How users define faults](#5-how-users-define-faults)
6. [How simulation data/workloads are generated](#6-how-simulation-dataworkloads-are-generated)
7. [State-space exploration](#7-state-space-exploration)
8. [The determinism contract (user obligations)](#8-the-determinism-contract)
9. [Roadmap](#9-roadmap)
10. [Risks](#10-risks)
11. [Repo layout](#11-repo-layout)

Full API reference: **`docs/design.md`**. Antithesis research: **`docs/antithesis-study-notes.md`**.

---

## 1. Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Language | **C++20** | User requirement; coroutines give ergonomic cooperative tasks with zero dependencies. |
| Delivery form | **Two libraries**: `libcosmos` (sim) + `libcosmos-real` (prod) | One app source, two binaries: a testing binary and a normal prod binary. |
| App runtime (Seam A) | App codes against abstract `Runtime` (`Clock`/`Rng`/`Net`/`Storage`); `Simulator` and `RealRuntime` both implement it | FoundationDB model: app never calls OS I/O directly → same source is the test binary *and* the prod binary. Structural from Phase 1. See `docs/architecture.md`. |
| Execution substrate (Seam B) | `ISubstrate` seam *inside* the sim backend; library substrate (coroutines) now, hypervisor substrate (KVM) later | The determinism engine is written once; the hypervisor becomes "implement `ISubstrate` for KVM", not a separate project. |
| Determinism model | **In-process, single-threaded simulation** (FDB/TigerBeetle model) for the library substrate | One deterministic cooperative scheduler + virtual time + seeded RNG + simulated I/O = reproducible-by-seed executions. |
| Task model | **C++20 stackless coroutines** on a deterministic scheduler (library substrate) | User code looks synchronous (`co_await net.recv()`), scheduler controls all interleavings. No threads inside a simulation. |
| Exploration | **Seeded fuzzing across many runs** (1 run = 1 universe), parallel across cores; coverage guidance + `Snapshot`-based branching later | Antithesis-style input tree implemented at decision-log level. |
| Hypervisor | **Future substrate** (`KvmSubstrate : ISubstrate`, Phase 7+) | Removes the determinism contract for unmodified binaries by interposition; reuses the entire engine via Seam B. |
| Production story | Same app source, two binaries: `myapp_test` (links `libcosmos`, runs `Campaign`) and `myapp` (links `libcosmos-real`, normal OS process) | The prod binary is real OS execution, never a determinism target. |

## 2. What "library-first DST" means

```
┌─ your application ─────────────────────────────────────────┐
│  your nodes/services/clients written against cosmos APIs:  │
│  spawn tasks, co_await sleep/recv, net.send, store.write,  │
│  rng draws, assertions                                     │
└──────────────┬─────────────────────────────────────────────┘
               │ never touches OS time/threads/sockets/rand
┌──────────────▼─────────────────────────────────────────────┐
│ libcosmos                                                  │
│  ┌───────────────┐  ┌──────────────┐  ┌─────────────────┐  │
│  │ deterministic │  │ virtual time │  │ seeded RNG      │  │
│  │ scheduler     │  │              │  │ streams         │  │
│  └───────────────┘  └──────────────┘  └─────────────────┘  │
│  ┌───────────────┐  ┌──────────────┐  ┌─────────────────┐  │
│  │ simulated net │  │ simulated    │  │ fault injector  │  │
│  │ (loss/delay/  │  │ storage      │  │ (profile +      │  │
│  │  partitions)  │  │ (fsync model)│  │  scripted +     │  │
│  │               │  │              │  │  custom hooks)  │  │
│  └───────────────┘  └──────────────┘  └─────────────────┘  │
│  ┌──────────────────────────────────────────────────────┐  │
│  │ campaign runner: N seeds in parallel, assertions,    │  │
│  │ repro-by-seed, trace export                          │  │
│  └──────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────┘
```

One `Simulator` instance = one universe. A **campaign** runs thousands of
universes with different seeds (across all cores), aggregates assertion
results, and prints a repro command (`--seed 12345`) for every failure.
Every failure replays **exactly**, because the whole universe is a function
of its seed.

## 3. Core design

Determinism comes from controlling five things, and nothing else:

1. **Scheduling** — a single-threaded cooperative scheduler runs all tasks
   (coroutines). When several tasks are runnable, the choice is made by a
   seeded RNG → interleavings are explored across seeds, replayed within one.
2. **Time** — a virtual clock that only advances when no task is runnable, to
   the next scheduled event. `now()`, `sleep()`, and all timer firings are
   deterministic.
3. **Randomness** — one master seed derives **independent streams**
   (`schedule`, `fault`, `workload`, `user`) so that e.g. adding workload
   operations doesn't perturb scheduling exploration. All draws are logged
   implicitly by the seed.
4. **I/O** — network and disk are simulated in-process: every send/write is an
   event delivered at a virtual time chosen via seeded draws; faults
   (drop, delay, reorder, partition, crash, disk error) are injected from the
   `fault` stream.
5. **Inputs** — anything entering the simulated world (workload operations,
   initial data, fault decisions) is generated from seeded generators or
   supplied by the harness as data → the seed fully determines the run.

Result: **seed ⇒ identical execution, instruction for instruction** (given the
same binary). That gives us the three DST superpowers at process level:

- **Perfect replay**: `cosmos run --seed S` reproduces any finding exactly.
- **Exploration**: many seeds = many universes; assertions find bugs.
- **(Later) branching**: `fork()` at choice points = cheap COW snapshots of a
  universe → Antithesis-style multiverse without a hypervisor.

## 4. Public API overview

Headers under `include/cosmos/`. Full reference with signatures and semantics
in `docs/design.md`. Summary:

| Header | Public surface | Purpose |
|---|---|---|
| `simulator.hpp` | `Simulator`, `SimConfig`, `SimResult` | The universe: `spawn`, `run`, `now`, `sleep`, `at`/`every`, rng streams, node management |
| `task.hpp` | `Task`, `co_await` awaitables (`Sleep`, `Yield`) | Cooperative coroutine tasks |
| `time.hpp` | `Time`, `Duration`, literals (`ms`, `s`) | Virtual time types |
| `random.hpp` | `Rng` (xoshiro256**), `coin`, `range`, stream splitting | Deterministic randomness |
| `net.hpp` | `Net`, `Node`, `Endpoint`, `Address`, `send`/`recv` | Simulated network with faults |
| `storage.hpp` *(Phase 4)* | `Storage`, `File`, `write`/`read`/`fsync` | Simulated disk with crash semantics |
| `faults.hpp` | `FaultProfile`, verdict hooks, `partition`, `crash` | Fault definition (see §5) |
| `gen.hpp` | `gen::range`, `gen::one_of`, `gen::string`, `gen::exponential`, … | Deterministic data generation (see §6) |
| `assert.hpp` | `cosmos::always`, `cosmos::sometimes`, `cosmos::reachable`, `COSMOS_CHECK` | Property assertions; `sometimes` evaluated campaign-wide |
| `campaign.hpp` | `Campaign`, `CampaignConfig`, `CampaignReport` | Multi-seed parallel exploration + repro instructions |
| `real.hpp` *(later)* | real-OS backends of the same interfaces | Ship the same code to production |

## 5. How users define faults

Four composable mechanisms (details in `docs/design.md` §6):

1. **Declarative profile** — rates applied by the `fault` RNG stream:
   ```cpp
   FaultProfile f;
   f.packet_loss   = 0.01;              // Bernoulli per packet
   f.latency       = gen::lognormal{1ms, 100ms};
   f.reorder_rate  = 0.05;
   f.crash_interval= gen::exponential{30s}; // random node crashes
   sim.set_faults(f);
   ```
2. **Imperative / scripted** — from workload or scheduled lambdas:
   ```cpp
   sim.net().partition({n0, n1}, {n2, n3});   // split brain
   sim.net().heal_all();
   sim.crash(n2);  sim.reboot(n2);
   sim.at(10s, [&]{ sim.crash(n2); });        // deterministic schedule
   ```
3. **Custom verdict hooks** — full programmatic control per packet/op:
   ```cpp
   sim.net().on_send = [&](const PacketView& p) -> Verdict {
       if (p.to == n3 && rng.coin(0.5)) return Drop{};
       return DeliverAfter{gen::range(rng, 1, 20) * 1ms};
   };
   ```
4. **Crash/durability semantics for storage** *(Phase 4)* — writes are not
   durable until `fsync`; crash-reboot discards un-synced data and may tear
   the last synced sector — controlled by the fault stream.

All fault decisions come from the seeded `fault` stream → fault timelines are
reproducible and are themselves part of the explored space.

## 6. How simulation data/workloads are generated

1. **Seeded generators** (`gen.hpp`): property-testing-style combinators
   (`range`, `one_of`, `string`, `weighted`, `exponential`…) drawing from the
   `workload` stream. Users write **client/workload actors** that issue
   operations built from these draws.
2. **Swarm configuration**: the campaign derives per-run config knobs
   (message rates, key space size, fault intensities) from the seed, so each
   universe stresses a different point of the configuration space.
3. **Fixtures**: initial state (e.g. preloaded keys) supplied by the harness
   at universe construction — plain data, part of the run config.
4. **(Later) recorded traces**: replay captured production traffic through
   the simulated world.

Because everything flows from the seed, "what data did this run use?" is
always answerable by replay.

## 7. State-space exploration

- **v1 (now)**: seeded fuzzing campaign. Each seed = one universe; the
  schedule stream explores interleavings, the fault stream explores failure
  scenarios, the workload stream explores inputs. Trials run in parallel
  across cores (each universe is single-threaded). Assertions:
  - `always(cond, id)` — any violation = finding, with seed + trace;
  - `sometimes(cond, id)` — must be hit in **at least one** universe of the
    campaign (liveness/coverage — Antithesis semantics);
  - findings are deduped by assertion id and printed with a repro command.
- **v2**: coverage-guided seed selection (clang/gcc sancov edge counts fed
  back to prefer novel seeds — AFL-style corpus of seeds).
- **v3**: `fork()`-based universe branching at choice points (cheap COW
  snapshots), input-decision-log minimization (delta-debug a failing run to a
  minimal repro), structured trace export (JSON/chrome-trace) for debugging.

## 8. The determinism contract

The library guarantees determinism **if** application code inside a
simulation obeys these rules (this replaces the hypervisor's enforcement —
that is the price of library-first):

1. No OS time (`std::chrono::system_clock`, `clock_gettime`) — use `sim.now()`.
2. No OS randomness (`rand()`, `std::random_device`, `/dev/urandom`) — use
   `sim.rng()` / `gen`.
3. No threads, no blocking OS calls, no real sockets/files — use cosmos
   tasks, net, storage. (CPU-bound work is fine.)
4. No iteration-order dependencies on pointer values / addresses (ASLR
   leaks): e.g. don't let `std::unordered_map<T*>` iteration order affect
   behavior; don't sort raw pointers where order matters.
5. Floating point is deterministic on a fixed binary/arch, but avoid
   `long double` / x87 and fast-math flags in behavior-relevant paths.
6. Anything with external effect (logging is fine) must stay out of the
   behavior path.

Violations typically show up as same-seed divergence — caught by the
double-run harness (`campaign --verify` mode: re-runs a sample of seeds and
bit-compares trace hashes).

## 9. Roadmap

| Phase | Content | Exit criteria |
|---|---|---|
| 0 | Study notes (Antithesis/FDB/TigerBeetle) — done, see `docs/antithesis-study-notes.md` | — |
| 1 | **Core runtime + two-binary split**: `Runtime` interface (Seam A), rng streams, virtual time, coroutine scheduler, `Universe`+`ISubstrate` (library substrate), simulator loop, assertions; **minimal real backend** (real clock + echo net) so the same app compiles into a prod binary | same seed ⇒ bit-identical trace hash; app builds as both `myapp_test` and `myapp` |
| 2 | **Simulated network + faults** (+ real-backend parity: real sockets via epoll): endpoints, latency/loss/reorder, partitions, crash/reboot, verdict hooks | ping-pong survives faults; partition reproducible; prod binary pings over real sockets |
| 3 | **Workloads + campaign**: gen combinators, swarm configs, parallel campaign runner, repro CLI, trace-hash verify mode | 10k-seed campaign across cores; findings replay exactly |
| 4 | **Simulated storage** (+ real files/fsync parity): write/fsync durability model, crash-torn writes | KV example detects lost-write bug under crash faults |
| 5 | **Exploration v2/v3**: sancov guidance, `Snapshot`-based branching (Seam B), decision-log minimization, trace export | minimized repros; visible multiverse map |
| 6 | **Production hardening**: harden the real backend to full parity, packaging, docs | example app runs reliably in prod mode |
| 7 | **(Optional) hypervisor substrate**: `KvmSubstrate : ISubstrate` — run unmodified binaries deterministically (original KVM plan) | reuses Phases 1–5 engine; gate is only the VM time/IO determinism work |

Timeline: Phases 1–3 ≈ 2–3 weeks part-time (the time-crunch MVP). The
two-binary split costs little: the load-bearing artifact is the `Runtime`
interface, not a complete real backend.

## 10. Risks

1. **Contract violations** (hidden OS calls in app or third-party code) →
   mitigated by the double-run verify mode and, later, interposition
   (`--wrap`/`LD_PRELOAD` traps for time/random APIs).
2. **Coroutine allocator/UB bugs** in the task runtime → keep the runtime
   tiny and heavily asserted; ASan/UBSan in CI.
3. **In-process nondeterminism leaks**: pointer-order iteration, ASLR,
   unordered containers with address-dependent hashes → documented contract +
   verify mode.
4. **Single-process ceiling**: one universe can't exceed one core's
   throughput — by design; scale by more universes, not bigger universes.
5. **Branching memory pressure** in Phase 5 (`Snapshot` save/restore) →
   bounded branching budget.
6. **Premature abstraction** — introducing the `ISubstrate` seam (Seam B) before
   the hypervisor exists means guessing its needs. Mitigated by keeping the
   interface minimal and deriving it from the library substrate first; expect
   one revision when `KvmSubstrate` lands. The Antithesis study notes give a
   strong prior on what a hypervisor substrate needs, so the guess is
   well-grounded.

## 11. Repo layout

```
cosmos/
├── PLAN.md                      ← this file
├── docs/
│   ├── architecture.md          ← seams, backends, substrates (the spine)
│   ├── design.md                ← full public API & semantics reference
│   └── antithesis-study-notes.md← research background
├── CMakeLists.txt               ← defines libcosmos, libcosmos-real, examples
├── include/cosmos/              ← public headers (Seam A interface + sim)
│   ├── cosmos.hpp  runtime.hpp  task.hpp  time.hpp  random.hpp
│   ├── simulator.hpp  net.hpp  faults.hpp  gen.hpp  assert.hpp  campaign.hpp
│   └── (storage.hpp — Phase 4)
├── include/cosmos-real/         ← real backend headers (Seam A impl, prod)
│   └── real.hpp                 ← RealRuntime (epoll, real sockets/files/time)
├── src/cosmos/                  ← libcosmos: Universe, ISubstrate, library substrate, sim backends
├── src/cosmos-real/             ← libcosmos-real: RealRuntime + epoll executor
└── examples/
    ├── ping_pong.cpp            ← shared app source (written against Runtime)
    ├── ping_pong_test.cpp       ← links libcosmos; runs Campaign (testing binary)
    └── ping_pong_prod.cpp       ← links libcosmos-real; normal OS process (prod binary)
```
