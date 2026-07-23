# Cosmos — Deterministic Simulation Testing via a Deterministic Hypervisor

A learning-first project to build an Antithesis-style deterministic simulation
testing (DST) system: a custom deterministic hypervisor that runs unmodified-ish
Linux guests, guarantees instruction-level determinism, and uses it for
state-space exploration, fault injection, perfect replay, and time-travel
debugging.

> Status: planning complete, implementation not started.
> This document is the project bible: study notes, decisions, roadmap, resources.

---

## Table of Contents

1. [Locked-in decisions](#1-locked-in-decisions)
2. [How Antithesis works (verified study notes)](#2-how-antithesis-works-verified-study-notes)
3. [The theory in one paragraph](#3-the-theory-in-one-paragraph)
4. [The nondeterminism budget](#4-the-nondeterminism-budget)
5. [Target architecture](#5-target-architecture)
6. [Roadmap](#6-roadmap)
   - [Phase 0 — Study](#phase-0--study-12-weeks)
   - [Phase 1 — Minimal VMM](#phase-1--minimal-vmm-23-weeks)
   - [Phase 2 — Boot Linux](#phase-2--boot-linux-24-weeks)
   - [Phase 3 — Determinization of one VM](#phase-3--determinization-of-one-vm-46-weeks--the-heart)
   - [Phase 4 — Platform channel & environment model](#phase-4--platform-channel--environment-model-34-weeks)
   - [Phase 5 — Snapshots, branching, replay](#phase-5--snapshots-branching-replay-46-weeks)
   - [Phase 6 — Exploration engine ("Director")](#phase-6--exploration-engine-director-ongoing)
   - [Phase 7 — SDK + CLI](#phase-7--sdk--cli)
7. [Cross-cutting engineering practices](#7-cross-cutting-engineering-practices)
8. [Reading / reference list](#8-reading--reference-list)
9. [Risks and open research questions](#9-risks-and-open-research-questions)
10. [Repo layout](#10-repo-layout)
11. [Timeline expectations](#11-timeline-expectations)
12. [Immediate next steps](#12-immediate-next-steps)

---

## 1. Locked-in decisions

| Decision | Choice | Rationale |
|---|---|---|
| Host platform | **Linux + KVM** | Dev machine is Linux (Intel i5-12450H, VMX present, `/dev/kvm` accessible, 12 threads, 15 GB RAM). Antithesis/dhyve used FreeBSD/bhyve; we follow their architecture, not their OS. |
| VMM approach | **Written from scratch** against `/dev/kvm` | Deep learning is the stated goal. kvmtool / rust-vmm / Firecracker serve as references, not bases. |
| Language | **Rust** | rust-vmm ecosystem for reference, memory safety for device models, Firecracker as a mature exemplar. |
| Project goal | **Deep learning journey** | From-scratch exercises over speed. Correctness of understanding > feature velocity. |
| Target workloads | **None specific yet** | Build the platform first; guest requirements stay minimal (no Docker-in-guest needed initially). |
| Core topology (from Antithesis) | **One whole system inside ONE VM; one VM pinned to ONE physical core** | Parallelism across cores for exploration, never multi-vCPU within a VM (inter-core interleaving kills instruction-level determinism). |

---

## 2. How Antithesis works (verified study notes)

Primary sources: their engineering blog posts, product page, and docs (links in
[section 8](#8-reading--reference-list)). Everything below is from those
materials, not speculation.

### 2.1 The "Determinator"

- A **fork of FreeBSD's bhyve** hypervisor, using **Intel VMX** hardware
  virtualization.
- Their first engineering move was **removing** standard hypervisor
  functionality. Real hardware leaks entropy everywhere; a deterministic
  hypervisor starts from a *small deterministic core* and grows outward
  incrementally. **Minimal device surface is their #1 design lesson.**
- Day-to-day work = categorizing every piece of micro-level CPU behavior as
  either *deterministic* (keep in guest, it's fast) or *nondeterministic*
  (must avoid, contain, or reverse). Assumptions are dangerous: only
  whole-system testing reveals what is actually deterministic.

### 2.2 Topology

- The **entire system under test** — all services, databases, client
  workloads, orchestrated by Docker Compose or Kubernetes — runs **inside a
  single VM**.
- Each hypervisor instance runs on **one physical CPU core**. A 96-core
  machine runs ~96 independent VMs, each exploring a different branch of the
  state space. Aggregate *throughput of exploration* matters; single-VM
  wall-clock speed does not.
- The unit of reproducibility is the **state of the whole system as an
  interconnected whole**, not any single process. No domain-specific mocks
  needed: client and server live in the same deterministic bubble.
- Guest software still experiences *concurrency* (threads, preemption) via the
  guest OS scheduler — but the platform controls that scheduler's inputs, and
  even uses it as a fault-injection lever (e.g. thread starvation).

### 2.3 Time determinism (the crux)

- Requirement: **the guest's simulated clock must be a function of only the
  deterministic state and execution history of the guest.**
- Every time source visible to the guest — TSC (RDTSC/RDTSCP), HPET, etc. —
  is intercepted and returns a virtual value computed by the hypervisor.
- They tried driving virtual time from the **instructions-retired PMC** and
  hit two walls:
  1. PMC instructions-retired isn't quite deterministic, even in precise mode:
     ~1 in a trillion instructions miscounted (speculated: pipelining /
     branch prediction / external interrupt handling quirks).
  2. PMC threshold interrupts arrive via the APIC with variable latency of
     dozens of instructions — you can't tell exactly when the threshold hit.
- The open-source **dhyve** instead drives virtual time from a **deterministic
  count of executed branch instructions** (the same class of counter rr uses
  for record/replay) and it works. We adopt that approach.
- Time nondeterminism converts into data nondeterminism (racing transactions,
  interleaved event delivery, reading the clock) — this is why time is the
  heart of the problem.

### 2.4 Deterministic I/O boundary

- A deterministic system connected to a nondeterministic channel is
  nondeterministic. So all host↔guest communication goes through one
  controlled channel built on the **`VMCALL` instruction** (roll-your-own
  instruction that just exits guest → host).
- Uses: guest emits logs/assertions/telemetry out; guest ingests commands and
  RNG seeds in.
- **Every point where the guest consumes external input is a possible branch
  point.** Externally, a test run is an **input tree**; exploration walks and
  forks that tree.
- Later addition: **interrupt injection** to "push" events into the guest
  preemptively (inputs then have an injection time rather than only being
  consumed at guest-chosen points).

### 2.5 Snapshots and the multiverse

- They **never replay from the beginning** to time-travel: the hypervisor
  supports **fast, efficient snapshotting** of full guest state.
- Snapshots enable: branch-the-past exploration (multiverse), rewind-and-
  inspect ("`sleep -5`" for a crashed server), retroactive debugging (attach
  a debugger to a process 5 seconds before its crash), retroactive packet
  capture/profiling, time compression (simulate idle periods faster),
  "change the past" experiments (rewind, disable a fault, see if the bug
  persists), and causality analysis (rewind N seconds, re-sample the bug's
  probability across many branches).

### 2.6 Exploration and fault injection

- Extreme chaos engineering inside the deterministic bubble: everything (SUT,
  client, checkers) runs in a fault-filled environment with fuzzed inputs.
- Guidance is **feedback-driven** (coverage + RL): seeks out novel system
  states rather than blindly repeating scenarios.
- Faults: network (partitions, loss, delay), storage faults, machine
  kill/pause, scheduling/thread starvation, clock manipulation — all with
  zero configuration required from the user.
- Bug detection = **user-defined properties/assertions** via their SDK
  (`always`, `sometimes`, `reachable`, …) plus coverage instrumentation;
  SDKs exist for many languages with a low-level fallback API over the
  hypercall channel.

### 2.7 Engineering practices worth copying

- **Hyperactive logging** was the only way to pin down subtle nondeterminisms:
  ~50 GiB of output per 20-minute run. FreeBSD's kernel log dropped messages
  under that load, so they wrote a custom kernel logger with large buffers and
  proactive pausing.
- **Hardware can betray you**: a software "bug" they chased was actually a
  faulty-RAM bit flip; their office workstations now use ECC memory. Expect
  rare false divergences on consumer hardware — re-run to confirm before
  hunting ghosts.
- The deterministic hypervisor is only **one building block**: exploration
  engine, snapshot/branch machinery, replay from cold storage, SDK, triage
  reports, and debugging UX are separate layers.

### 2.8 The open-source proof: dhyve

[**dhyve**](https://github.com/pgraug/dhyve-src) — a DTU bachelor's project by
two students, explicitly inspired by Antithesis:

- Fork of bhyve (Intel VMX); virtual time from a **deterministic count of
  executed branch instructions**.
- Hardware randomness, timer interrupts, and I/O intercepted and emulated
  deterministically.
- **Director**: snapshot/branch/mutation harness for state-space exploration.
- Alpine Linux guest with **kernel patches** and test layers; `dhv` CLI +
  `dhvd` daemon for building, testing, inspecting deterministic runs.
- Repo structure: `dhyve/` (modified hypervisor), `guest/` (image build,
  kernel patches), `dhv/` (CLI + daemon).

Two students built a working version in a semester *by forking an existing
VMM*. We choose from-scratch (slower) deliberately, for depth of learning.

---

## 3. The theory in one paragraph

> If the guest's clock and entropy are **pure functions of its own
> deterministic execution history**, and **every other input** arrives through
> a **sequenced, replayable channel**, then the whole VM is deterministic.
> Determinism then unlocks the three superpowers: **perfect replay**
> (re-run any execution exactly), **branching exploration** (snapshot → fork
> universes → fuzz the input tree), and **time travel** (restore any snapshot,
> inspect or mutate the past).

Everything in this project is in service of those three properties.

---

## 4. The nondeterminism budget

Every source of nondeterminism visible to the guest must be intercepted and
replaced with a deterministic function of guest execution history. This is the
master checklist; the divergence journal (section 7) tracks found & fixed
items.

| # | Source | Examples | Neutralization strategy |
|---|--------|----------|-------------------------|
| 1 | Time reads | RDTSC, RDTSCP, RDTSCP via vDSO, HPET MMIO, ACPI PM timer, RTC, TSC-deadline mode | Intercept all; return virtual time derived from deterministic branch-instruction count |
| 2 | Timer interrupts | LAPIC timer, PIT, HPET interrupts | Drive all timer interrupts from virtual-time deadlines in the VMM; don't create in-kernel PIT |
| 3 | Entropy instructions | RDRAND, RDSEED | Mask in CPUID (`KVM_SET_CPUID2`); trap if reachable; feed seeded PRNG via virtio-rng |
| 4 | Kernel entropy | getrandom(), jitter entropy, /dev/urandom | Seeded virtio-rng + small guest kernel patches (dhyve's approach) |
| 5 | Interrupt timing | Device IRQs, IPIs | All interrupts become virtual events delivered at deterministic virtual times |
| 6 | Device completion timing | Disk I/O done, net packet RX | Completions scheduled at virtual-time deadlines; content from deterministic sources (images, seeded model) |
| 7 | Multi-vCPU interleaving | SMP memory-ordering races | **Banned by design**: exactly 1 vCPU per VM. Guest thread concurrency remains via guest OS scheduling (which is deterministic once 1–6 are closed) |
| 8 | CPUID / feature variance | Frequency leaves (0x15/0x16), host-model-dependent features | Normalize CPUID to a fixed synthetic feature set |
| 9 | Host noise | Host scheduling, host timers, SMI, thermal | Can't leak *values* once 1–8 are closed (they only affect wall-clock speed); still pin vCPU to a core for sanity |
| 10 | Physical memory errors | Bit flips | Out of scope to prevent; re-run divergences to confirm before debugging (Antithesis's ECC lesson) |
| 11 | Guest async kernel activity | kworkers, async IO completions | Virtual-time-driven where visible; guest kernel patches where necessary |

**Design rule:** vCPUs never observe the host's wall clock, real entropy, or
real device timing. Only wall-clock *throughput* varies; guest-observable
*state* never does.

---

## 5. Target architecture

```
┌─────────────────────────────── host (Linux, KVM) ──────────────────────────────┐
│                                                                                │
│  director/                        one VM per physical core (pinned)            │
│  ┌────────────────────┐         ┌──────────────────────────────────────┐       │
│  │ exploration engine │────────▶│ vmm/ (Rust, from scratch, /dev/kvm)  │       │
│  │ - universe tree    │  many   │  - 1 vCPU                            │       │
│  │ - input tree walk  │         │  - virtual clock (branch-count)      │       │
│  │ - snapshot/branch  │         │  - UART, virtio-console/blk/rng      │       │
│  │ - fault scheduler  │         │  - hypercall channel (VMCALL exit)   │       │
│  │ - findings/minimize│         │  - snapshot/restore                  │       │
│  └────────────────────┘         │  - checkpoint hashing                │       │
│           ▲                     └───────────────┬──────────────────────┘       │
│           │ logs/assertions/coverage            │                              │
│           │                         ┌───────────▼──────────────┐               │
│           │                         │ guest (Linux, patched)   │               │
│           │                         │ - agent: fault injection │               │
│           │                         │   (netem, kill, pause)   │               │
│           │                         │ - system under test      │               │
│           │                         │ - sdk/ client lib        │               │
│           └─────────────────────────┤   (assertions, seeded    │               │
│              hypercall channel      │    randomness)           │               │
│                                     └──────────────────────────┘               │
└────────────────────────────────────────────────────────────────────────────────┘
```

Key architectural choice (from Antithesis): **the "network" is not a
hypervisor device problem.** Run all nodes of the SUT as processes/containers
inside ONE guest; the inter-node network is ordinary in-guest Linux
(namespaces, netem, iptables) driven deterministically by the guest agent over
the seeded control channel. This collapses most of the device-determinism
problem into a guest-software problem.

---

## 6. Roadmap

### Phase 0 — Study (1–2 weeks)

Read before writing code, in this order:

1. **Antithesis corpus** (links in section 8): deterministic hypervisor post →
   multiverse debugging → "How Antithesis works" → launch post → findability →
   causality analysis. Watch Alex Pshenichkin's FreeBSD Dev Summit talk.
2. **dhyve end-to-end**: clone it; read the VMM diff, the guest kernel
   patches, the Director harness. Map every feature to the nondeterminism
   source it kills (section 4).
3. **DST philosophy**: FoundationDB's simulation (Will Wilson's StrangeLoop
   talk) and TigerBeetle's VOPR docs — exploration tactics, assertion design,
   why determinism changes debugging economics.
4. **rr**: design docs/papers — process-level record/replay; the
   retired-conditional-branches counter trick; how they catalogue and contain
   nondeterminism.
5. **QEMU icount + record/replay docs**: vocabulary and proof that
   deterministic full-system emulation is achievable.

**Deliverable:** written nondeterminism budget (section 4, refined) +
`docs/journal.md` started.

### Phase 1 — Minimal VMM (2–3 weeks)

Write `vmm/` from scratch in Rust against `/dev/kvm` (raw ioctls or the
`kvm-ioctls` crate):

- `KVM_CREATE_VM`, `KVM_SET_USER_MEMORY_REGION`, `KVM_CREATE_VCPU`,
  `KVM_GET/SET_REGS`, `KVM_GET/SET_SREGS`, the `KVM_RUN` loop with exit
  handling (`KVM_EXIT_IO`, `KVM_EXIT_MMIO`, `KVM_EXIT_HLT`).
- Boot a hand-written **16-bit real-mode** guest that prints via port I/O.
- Then a **64-bit long-mode** guest: build page tables, GDT, enter long mode,
  print over a 16550 UART you emulate (PIO exits).
- References: KVM API doc, kvmtool source, rust-vmm/vmm-reference, Firecracker
  source, Intel SDM Vol. 3C.

**Success test:** guest echoes characters over the emulated UART; all exits
handled by your Rust code.

### Phase 2 — Boot Linux (2–4 weeks)

- Implement the **x86 Linux boot protocol**: zero page / boot params, E820
  memory map, load bzImage + initramfs, `console=ttyS0`.
- **virtio-mmio** transport (deliberately skip PCI — far simpler):
  virtio-console first, then virtio-block (split virtqueues, descriptors,
  avail/used rings, interrupt via in-kernel irqchip).
- Use KVM's **in-kernel irqchip** (LAPIC/IOAPIC) like Firecracker; userspace
  keeps control of *when* interrupts are injected.
- Guest: minimal kernel config + busybox initramfs; run a statically linked
  hello-world.

**Success test:** boot to a shell prompt; read/write a block device.

### Phase 3 — Determinization of one VM (4–6 weeks) ← THE HEART

**Definition of done:** two runs with the same seed produce bit-identical
serial output **and** identical guest-RAM + vCPU-state hashes at every
virtual-time checkpoint.

Work items:

1. **Virtual clock (research spike — the project's crux):** advance time only
   from a deterministic counter. Options, in preference order:
   a. **Retired-branch count** via `perf_event_open` on the vCPU task with
      guest-only counting (dhyve's proven model);
   b. **Small out-of-tree KVM patch** (we control our kernel — same spirit as
      Antithesis patching bhyve and dhyve patching its guest);
   c. Coarse v1 fallback: advance virtual time only at VM-exits, by
      deterministic amounts.
   Budget real time here; keep the journal.
2. **Time sources:** intercept/virtualize RDTSC/RDTSCP, HPET, ACPI PM timer,
   RTC; deliver **all** timer interrupts from virtual-time deadlines computed
   by the VMM; don't create the in-kernel PIT. (Clean RDTSC-exiting may
   require the KVM patch — known kernel work.)
3. **Entropy:** mask RDRAND/RDSEED in guest CPUID; provide seeded PRNG via
   virtio-rng; normalize CPUID (fixed synthetic feature set, hide frequency
   leaves); guest kernel patch for getrandom/jitter entropy if needed.
4. **Harness:** checkpoint hashing (RAM + regs every N virtual-ms), a
   same-seed double-run comparator, hyperactive exit/interrupt logging for
   divergence hunting, and the **divergence journal** (every nondeterminism
   found, root-caused, fixed — the best learning artifact of the project).

### Phase 4 — Platform channel & environment model (3–4 weeks)

- **Hypercall control channel:** guest `VMCALL` → `KVM_EXIT_HYPERCALL` (or
  unknown-hypercall exit) in userspace. Protocol v1: guest→host log/assertion
  records; host→guest commands + RNG seeds. **Log every input consumption as a
  sequenced input record** — these are the branch points of the input tree.
- **Environment model:** all SUT nodes as processes (later: containers) inside
  the one guest; inter-node network via in-guest namespaces/netem; **guest
  agent** executes seeded fault commands: partition, delay, loss, duplicate,
  kill -9, pause/resume (SIGSTOP), disk error injection, CPU/thread
  starvation.
- **Deterministic virtio-block:** completions at virtual-time deadlines;
  copy-on-write over read-only base images.

**Success test:** a 3-node toy system (e.g. ping-pong over TCP between
namespaces) runs identically twice under identical seeds, including identical
fault timelines.

### Phase 5 — Snapshots, branching, replay (4–6 weeks)

- **Snapshot:** vCPU regs/sregs/xsave/`KVM_GET_VCPU_EVENTS`, `KVM_GET_LAPIC`,
  full RAM dump, device state (ours — small by design), TSC offset. Serialize
  to disk.
- **Branch:** restore a snapshot N times with a different next input record →
  N universes.
- **Replay:** seed + input log → exact re-execution; verify against checkpoint
  hashes.
- **Time travel:** snapshot ring buffer → "rewind 5 s and open a shell in that
  universe" (the Multiverse Debugging demo). GDB stub in the VMM later.

**Success test:** run 60 virtual-seconds, snapshot every second; from any
snapshot, replay any recorded input sequence bit-identically; mutate one input
and observe divergence exactly at the mutation point.

### Phase 6 — Exploration engine ("Director") (ongoing)

- Universe-tree scheduler: one VM per pinned core, work-stealing over a shared
  tree of (snapshot, input-log) nodes.
- Guidance v1: random + novelty heuristics (new assertion/coverage events).
  Guidance v2: coverage-guided — guest coverage streamed over the channel
  (kcov-style or compiler-instrumented), prioritized like Antithesis's
  feedback-guided exploration.
- Findings pipeline: assertion failures → dedup by signature → **input-log
  minimization** (delta-debug the input tree to a minimal repro).

### Phase 7 — SDK + CLI

- Tiny Rust guest SDK first: `assert_always`, `assert_sometimes`,
  `reachable`, seeded `random()`, lifecycle events — all over the hypercall
  channel. (Fallback wire format first, ergonomic macros later; other
  languages much later.)
- CLI: `cosmos run --seed S`, `cosmos reproduce <finding>`, `cosmos rewind
  <moment>`, `cosmos shell <moment>`.

---

## 7. Cross-cutting engineering practices

- **Determinism harness from day one.** Never add a device or feature without
  a same-seed double-run bit-compare test. Determinism regressions are found
  immediately or never.
- **Hyperactive logging while hunting.** Antithesis needed ~50 GiB/20-min of
  logs to disentangle subtle nondeterminisms; don't be stingy with trace
  output, and make it lossless (their custom-logger lesson).
- **Divergence journal** (`docs/journal.md`): every divergence → suspected
  source → experiment → root cause → fix. This doubles as the learning log.
- **Re-run before you believe a divergence.** Bit flips happen on non-ECC
  consumer RAM (Antithesis got burned; we run on a laptop).
- **P/E-core discipline:** the i5-12450H is hybrid Alder Lake. Pin VMs to one
  core type when comparing runs or counting instructions/branches across runs;
  counter semantics can differ between core types.
- **Remove, don't add.** Every device/feature is a determinism liability.
  Default answer to "should the VMM emulate X?" is no until proven needed.
- **One vCPU per VM, forever.** Exploration parallelism = more VMs, never
  more vCPUs.

---

## 8. Reading / reference list

### Antithesis (primary sources — read first)

- [So you think you want to write a deterministic hypervisor?](https://antithesis.com/blog/deterministic_hypervisor/) — the core post (Alex Pshenichkin).
- [Debugging in the Multiverse](https://antithesis.com/blog/multiverse_debugging/) — snapshots, time travel, change-the-past debugging (Will Wilson).
- [How Antithesis works](https://antithesis.com/docs/introduction/how_antithesis_works/) — platform overview: input tree, RL guidance, properties.
- [Is something bugging you?](https://antithesis.com/blog/is_something_bugging_you/) — launch post, vision & motivation.
- [How Antithesis finds bugs (with help from Super Mario Bros.)](https://antithesis.com/blog/sdtalk/) — approachable exploration explainer.
- [The worst bug we faced at Antithesis](https://antithesis.com/blog/worst_bug/) — war story, includes the hardware-bit-flip lesson.
- [Did you get lucky or unlucky?](https://antithesis.com/blog/2025/findability/) — findability / bug probability.
- [When did the bug start?](https://antithesis.com/blog/2026/causality_analysis/) — causality analysis via rewind-and-resample.
- [The Antithesis environment](https://antithesis.com/docs/configuration/the_antithesis_environment/) and [Fault injection docs](https://antithesis.com/docs/concepts/fault_injection/) — environment model and fault taxonomy.
- Alex Pshenichkin, FreeBSD Developer Summit talk: `youtube.com/watch?v=0E6GBg13P60` (linked from the multiverse post; covers snapshotting).
- Product page: [antithesis.com/product](https://antithesis.com/product/) — "deterministic down to the instruction stream", one core per run, massively parallel universes.

### Open-source deterministic hypervisor

- [dhyve](https://github.com/pgraug/dhyve-src) — the closest existing artifact to this project. Read `dhyve/` (bhyve diff), `guest/` (Alpine + kernel patches), `dhv/` (Director harness, CLI/daemon). BSD-2-Clause.

### KVM / VMM construction

- KVM API: `kernel.org/doc/html/latest/virt/kvm/api.html` (also in kernel source: `Documentation/virt/kvm/api.rst`).
- [kvmtool](https://github.com/kvmtool/kvmtool) — minimal readable C VMM.
- [Firecracker](https://github.com/firecracker-microvm/firecracker) — production minimal Rust VMM.
- [rust-vmm](https://github.com/rust-vmm) crates and `rust-vmm/vmm-reference` — template VMM in Rust.
- crosvm (Chromium OS VMM) — another mature Rust reference.
- Intel SDM Vol. 3C (VMX), Vol. 3A (APIC/interrupts); `felixcloutier.com/x86` for instruction-level semantics (VMCALL, RDTSC, …).
- virtio spec (OASIS): split virtqueues, mmio transport — `docs.oasis-open.org/virtio`.
- Linux x86 boot protocol: kernel source `Documentation/arch/x86/boot.rst`.

### Deterministic execution / record-replay prior art

- [rr](https://rr-project.org/) — process-level deterministic record/replay; retired-conditional-branches counter; chaos mode scheduling.
- QEMU `icount` + record/replay docs — deterministic full-system emulation and reverse debugging.
- FoundationDB deterministic simulation: Will Wilson's StrangeLoop talk "Testing a Distributed System" and FoundationDB's testing documentation.
- TigerBeetle's simulator ("VOPR") docs and talks (Joran Dirk Greef) — single-threaded in-process DST design, fault taxonomy, exploration tactics.
- Marc Brooker's blog posts on simulation testing (AWS perspective).

### DST / property-based testing methodology

- Antithesis docs: [properties & assertions](https://antithesis.com/docs/concepts/properties_assertions/overview/), [reliability properties catalog](https://antithesis.com/docs/resources/deterministic_simulation_testing/), KV and blockchain property catalogs — for SDK/assertion design later.
- Jepsen writeups (aphyr.com) — what distributed-systems bugs look like; informs the fault taxonomy and property design.

---

## 9. Risks and open research questions

Ordered by severity:

1. **Deterministic virtual time on KVM.** dhyve counts branches in a kernel
   hypervisor module; on KVM we must count from userspace (perf on the vCPU
   task) or patch KVM. Antithesis's 1-in-a-trillion PMC miscount warning
   applies. *Mitigation: Phase 3 spike with three options (perf / kernel patch
   / exit-count fallback); the journal documents the verdict.*
2. **RDTSC interception.** Clean RDTSC-exiting may not be exposed by
   mainline KVM per-VM; a small kernel patch may be required. Acceptable (we
   own our kernel), but budget for it.
3. **Guest kernel entropy & async activity** (getrandom, jitter entropy,
   kworkers). *Mitigation: small guest kernel patches + seeded virtio-rng —
   dhyve's precedent.*
4. **Scope creep on device surface.** The #1 way this project dies. *Rule:
   UART + virtio-console/blk/rng + hypercall channel. Nothing else without a
   determinism plan and a double-run test.*
5. **PMC/counter behavior across P/E cores** on Alder Lake. *Pin to one core
   type for comparable runs.*
6. **Time budget.** From-scratch on KVM is slower than dhyve's fork-a-semester
   path. That's the chosen trade (learning-first); guard against despair by
   celebrating the Phase 1–3 milestones.

Open questions to resolve in Phase 3 (record answers in the journal):

- Which branch/instruction counter is exact enough, and how do we prove it?
  (Answer format: N same-seed runs × M checkpoints with zero divergence.)
- What is the cheapest interrupt-delivery discipline that is provably
  deterministic (virtual-time-driven injection points)?
- How much guest kernel patching is actually needed vs. CPUID masking +
  virtio-rng?

---

## 10. Repo layout

```
cosmos/
├── PLAN.md              ← this file
├── docs/
│   └── journal.md       ← divergence journal + design decisions log
├── vmm/                 ← Rust VMM (from scratch, /dev/kvm)
├── guest/               ← kernel config/patches, initramfs, guest agent
├── director/            ← exploration engine (snapshots, branching, guidance)
└── sdk/                 ← guest SDK (Rust first): assertions, seeded random
```

## 11. Timeline expectations

~5–7 months part-time to a deterministic single VM + basic exploration
(through Phase 5/early 6), longer for polish. Calibration points: dhyve was a
2-student semester project *forking* bhyve; we chose the slower from-scratch
path deliberately, for depth.

## 12. Immediate next steps

1. Scaffold the repo layout above (empty crates + `docs/journal.md`).
2. Phase 0 reading (section 6, Phase 0) — capture notes in the journal.
3. Start Phase 1: Rust binary that opens `/dev/kvm`, creates a VM, and runs a
   16-bit real-mode guest printing through a port.
