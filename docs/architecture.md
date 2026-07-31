# Cosmos — architecture: seams, backends, substrates

This is the **structural spine** of Cosmos. It defines the layering that lets
Cosmos be (a) a deterministic simulation library today, (b) a deterministic
hypervisor later, and (c) the *same application source* shipped as a normal
production binary — all from one codebase. Public API lives in
`docs/design.md`; this file explains *how the pieces are isolated and why*.

## 1. Thesis

The simulated system has to execute *somewhere*. That "somewhere" is the only
thing that changes between simulation, hypervisor, and production. Everything
else — seed, virtual clock, RNG streams, fault timeline, assertions, the
campaign/exploration engine, repro-by-seed — is shared and execution-agnostic.
Cosmos is built around **two seams** that make that sharing explicit.

## 2. Layered model

```
┌─────────────────────────────────────────────────────────────────┐
│  Application source (shared)                                    │
│  actors written against cosmos::Runtime (coroutines)            │
│  co_await rt.sleep / rt.net().send / rt.rng()  — never the OS   │
└────────────────────────────┬────────────────────────────────────┘
                             │  Seam A: app-facing Runtime
┌────────────────────────────▼────────────────────────────────────┐
│  cosmos::Runtime  (Clock, Rng, Net/Endpoint, Storage/File,       │
│                    Task + awaitables)                            │
└─────────┬──────────────────────────────────────────┬────────────┘
          │                                            │
┌─────────▼──────────────────┐         ┌──────────────▼──────────────┐
│  Sim backend (libcosmos)   │         │  Real backend (libcosmos-real)│
│  sim Clock/Rng/Net/Storage │         │  OS Clock/Rng/sockets/files  │
│  + Universe + faults +     │         │  + epoll executor            │
│    campaign                │         │  NOT deterministic — it is   │
│  DETERMINISTIC             │         │  production                  │
└─────────┬──────────────────┘         └──────────────┬──────────────┘
          │  Seam B: ISubstrate                        │
┌─────────▼──────────────────┐                        │
│  Library substrate         │  ← future:             │  (no engine;
│  (cooperative coroutine    │    Hypervisor substrate│   prod is not
│   scheduler = the "guest") │    (KVM/bhyve VM)      │   driven)
└────────────────────────────┘                        │
          │                                            │
┌─────────▼──────────────────┐         ┌──────────────▼──────────────┐
│  Testing binary myapp_test │         │  Prod binary myapp           │
│  links libcosmos; runs     │         │  links libcosmos-real;       │
│  Campaign                  │         │  normal OS process           │
└────────────────────────────┘         └─────────────────────────────┘
```

## 3. The two seams

**Seam A — Runtime backend (app-facing).** The abstractions the *application*
codes against: `Runtime` (`Clock`/`Rng`/`Net`/`Storage` + `Task`/awaitables).
Two implementations: the **Sim backend** (`libcosmos`: simulated time/net/disk
+ the determinism engine, for the testing binary) and the **Real backend**
(`libcosmos-real`: real OS time/sockets/files + an epoll executor, for the prod
binary). Seam A makes "one source, two binaries" possible: the app never calls
the OS directly, so the same source compiles against either backend. It is
**structural from Phase 1**, not a Phase-6 afterthought.

**Seam B — Execution substrate (engine-facing, inside the sim backend only).**
The interface the determinism engine drives to *run* a universe:
`ISubstrate` (`create_node`/`crash`/`reboot`, `run_until(time)`,
`deliver(node,channel,packet)`, `inject_fault`, `save`/`restore`). Two
implementations, both *inside* the sim backend: the **library substrate**
(cooperative coroutine scheduler; the "guest" is the app's own coroutines;
determinism is free) and the **hypervisor substrate** (`KvmSubstrate`,
future: a real VM running unmodified binaries; determinism by interposition;
no determinism contract needed).

The real backend does **not** implement Seam B — production is not driven by a
determinism engine. (Earlier drafts loosely called the real backend a
"substrate"; it is not. Only sim execution units are substrates.)

## 4. Backend (Seam A) — two binaries from one source

The application writes actors against `cosmos::Runtime&`:

```cpp
cosmos::Task server(cosmos::Runtime& rt, Address me) {
    auto& ep = rt.net().bind(me);
    for (;;) {
        co_await rt.sleep(rt.rng().exponential(5ms));
        auto [from, msg] = co_await ep.recv();
        ep.send(from, handle(msg, rt.rng()));
    }
}
```

Two `main`s, one source:

```cpp
// myapp_test.cpp — links libcosmos
int main() {
    cosmos::Simulator sim{/*SimConfig*/};
    build_cluster(sim);                              // spawn `server` actors
    auto report = cosmos::Campaign::run(cfg, build_cluster);  // deterministic
    // print findings + repro seeds
}

// myapp_prod.cpp — links libcosmos-real
int main() {
    cosmos::RealRuntime rt;                          // epoll, real sockets, real time
    build_cluster(rt);                               // SAME actors, real OS
    rt.run();                                        // normal process
}
```

`Simulator` *is a* `Runtime` (keeps its full ergonomic surface from
`design.md` §6) and *also* is the `Universe` facade. `RealRuntime` is the
other `Runtime`. The app sees only `Runtime`, so it is identical in both
binaries.

**The determinism contract becomes a structural property.** Under the sim
backend the app must be deterministic — but it is, *because the only clock/
rng/net it can reach are the abstract ones*. The two-binary split *enforces*
the contract by construction: you cannot accidentally call `clock_gettime`
when your only clock is `Runtime::now()`. The contract still has teeth for
*third-party* code that bypasses `Runtime` (the hypervisor removes even that).

## 5. Substrate (Seam B) — the hypervisor door

```cpp
class ISubstrate {
public:
    virtual NodeId create_node(std::string name) = 0;
    virtual void   crash(NodeId)  = 0;
    virtual void   reboot(NodeId) = 0;
    virtual void   run_until(Time deadline_or_quiescence) = 0;
    virtual void   deliver(NodeId to, ChannelId, PacketView) = 0;
    virtual void   inject_fault(NodeId, FaultEvent) = 0;
    virtual Snapshot save() = 0;
    virtual void   restore(Snapshot&&) = 0;
};
```

`Universe` (seed, virtual clock, RNG streams, fault timeline, assertions,
event loop, campaign) is written against `ISubstrate` and never knows whether
`run_until` resumes a coroutine or enters a vCPU. The library substrate
implements `run_until` = "drain the ready-coroutine queue"; the hypervisor
substrate implements it = "enter the VM until self-stop / branch-budget /
deadline". Everything above the seam — campaign, exploration, repro, triage —
is written once.

The library substrate is the cheap, deterministic-by-cooperation version that
validates the entire shared engine first; the hypervisor substrate swaps in
later and reuses all of it. *The library substrate is just the hypervisor
substrate with a cooperative-coroutine guest instead of a VM.*

## 6. Snapshot interface (branching)

Exploration v3 forks a universe at a choice point. That primitive is abstracted
behind `Snapshot` (`save()`/`restore()`):
- **Library substrate**: copy the in-process universe state (deterministic and
  bounded — no `fork()` needed; cleaner and works where `fork()` cannot).
- **Hypervisor substrate**: VM snapshot/restore (KVM/bhyve).

The decision-log + branch machinery above the seam is shared.

## 7. Determinism, per backend

| | App modified? | Time source | Threads | Determinism | Contract? |
|---|---|---|---|---|---|
| Sim / library substrate | yes (coroutines) | virtual, quiescence-driven | none (cooperative) | by cooperation | yes (enforced by `Runtime`) |
| Sim / hypervisor substrate | **no** | trapped TSC/HPET → branch-count | real guest threads, pinned to 1 core | by interposition | **no** |
| Real backend (prod) | yes (same source) | real OS clock | real OS threads | none — it is production | n/a |

Within a substrate, determinism = same seed ⇒ identical event order.
Cross-substrate trace equality is *not* a goal.

## 8. Ownership

| Concept | Lives in | Shared? |
|---|---|---|
| seed, RNG streams, virtual clock, fault timeline, assertions, campaign, exploration, repro, triage | determinism engine (`Universe`) | yes (all sim) |
| `Runtime`/`Clock`/`Rng`/`Net`/`Storage` interfaces, `Task`+awaitables | Seam A | yes (sim + real) |
| sim Clock/Rng/Net/Storage impls, `Universe`, `Simulator` | Sim backend (`libcosmos`) | — |
| real Clock/Rng/Net/Storage impls, `RealRuntime`, epoll executor | Real backend (`libcosmos-real`) | — |
| coroutine scheduler | Library substrate (Seam B) | — |
| VM, VNIC, VDisk, branch-count clock | Hypervisor substrate (Seam B, future) | — |
