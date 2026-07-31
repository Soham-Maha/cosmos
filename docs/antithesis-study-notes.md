# Antithesis — study notes (verified from primary sources)

Background research for Cosmos. Sources: Antithesis engineering blog, product
page, and docs (July 2026). Although Cosmos is now a library (not a
hypervisor), these notes capture the ideas we reuse: input tree, seeded
exploration, assertions, snapshots/branching, replay, time travel.

## The Determinator (their deterministic hypervisor)

- Fork of FreeBSD **bhyve**, Intel VMX. First move: **remove** functionality —
  start from a small deterministic core, grow incrementally. Minimal device
  surface is their #1 design lesson.
- Work = categorizing every micro CPU behavior as *deterministic* (keep in
  guest) or *nondeterministic* (avoid/contain/reverse). Only whole-system
  testing reveals which is which — assumptions are dangerous.

## Topology

- The **entire system under test** (all containers, Docker Compose / K8s) runs
  inside **one VM**, pinned to **one physical core**. Parallelism = many VMs
  exploring different branches; never multi-vCPU within a VM (inter-core
  interleaving kills instruction-level determinism).
- The unit of reproducibility is the **whole interconnected system state**, not
  a single process → no domain-specific mocks; client and server share one
  deterministic bubble.
- Guests still experience concurrency (thread preemption) via the guest OS
  scheduler — which the platform controls and uses for fault injection
  (thread starvation).

## Time determinism (the crux)

- Rule: **the guest clock must be a function of only the deterministic state
  and execution history of the guest.**
- All time sources (TSC/RDTSC, HPET, …) intercepted; virtual time returned.
- They tried instructions-retired PMC: (1) ~1-in-a-trillion miscounts even in
  precise mode; (2) PMC threshold interrupts arrive via APIC with variable
  latency. The open-source **dhyve** instead uses a **retired branch
  instruction count** (same trick as rr).
- Time nondeterminism becomes data nondeterminism (races, event interleaving,
  clock reads) — hence time is the heart of the problem.

## Deterministic I/O boundary

- One controlled channel (custom `VMCALL` hypercall): logs/assertions out,
  commands + RNG seeds in.
- **Every point where the guest consumes external input is a possible branch
  point.** A run is an **input tree**; exploration walks/forks that tree.
  (Cosmos equivalent: the seed-derived decision streams.)
- Later: interrupt injection to "push" events preemptively.

## Snapshots & the multiverse

- Fast full-guest snapshots → never replay from the beginning.
- Enables: branch-the-past exploration; rewind-inspect ("`sleep -5`");
  retroactive debugger attach / packet capture / profiling; time compression
  of idle periods; "change the past" experiments; causality analysis (rewind N
  seconds, resample bug probability across branches).
- Cosmos analogue: `fork()` COW snapshots + decision logs (Phase 5).

## Exploration & fault injection

- Extreme chaos engineering inside the deterministic bubble; fuzzed inputs +
  **feedback-guided** exploration (coverage + RL) seeking novel states.
- Faults: network partitions/loss/delay, storage faults, machine kill/pause,
  scheduling starvation, clock manipulation — zero-config.
- Detection = **user-defined properties** via SDK: `always`, `sometimes`,
  `reachable`, plus coverage instrumentation. `sometimes` assertions are
  evaluated **campaign-wide** (must fire at least once across all universes).

## Engineering practices

- **Hyperactive logging** (~50 GiB / 20-min run) was the only way to pin down
  subtle nondeterminisms; they wrote a custom lossless kernel logger.
- Hardware betrays: a chased "software bug" was a RAM bit flip → ECC
  workstations. On consumer hardware, re-run before believing a divergence.
- The hypervisor is only one building block: exploration engine, snapshot/
  branch machinery, SDK, triage reports, debugging UX are separate layers.

## dhyve (open-source proof)

- [github.com/pgraug/dhyve-src](https://github.com/pgraug/dhyve-src): DTU
  bachelor project (2 students, one semester): bhyve fork with branch-count
  virtual time; intercepted randomness/timers/I/O; **Director** snapshot/
  branch/mutation harness; Alpine guest + kernel patches; `dhv` CLI/daemon.
- Repos: `dhyve/` (hypervisor diff), `guest/`, `dhv/`. Proof a minimal DST
  platform is achievable with student-scale effort.

## Key links

- [So you think you want to write a deterministic hypervisor?](https://antithesis.com/blog/deterministic_hypervisor/)
- [Debugging in the Multiverse](https://antithesis.com/blog/multiverse_debugging/)
- [How Antithesis works](https://antithesis.com/docs/introduction/how_antithesis_works/)
- [Is something bugging you?](https://antithesis.com/blog/is_something_bugging_you/)
- [The worst bug we faced at Antithesis](https://antithesis.com/blog/worst_bug/)
- [Did you get lucky or unlucky? (findability)](https://antithesis.com/blog/2025/findability/)
- [When did the bug start? (causality analysis)](https://antithesis.com/blog/2026/causality_analysis/)
- [Fault injection docs](https://antithesis.com/docs/concepts/fault_injection/)
- Alex Pshenichkin's FreeBSD Dev Summit talk: `youtube.com/watch?v=0E6GBg13P60`

## Other prior art (library-level, closest to Cosmos)

- **FoundationDB simulation** — the original in-process DST: single-threaded
  deterministic runtime (Flow), simulated network/disk/time, swarm testing,
  "bugs found in simulation never seen in production". Will Wilson's
  StrangeLoop talk "Testing a Distributed System".
- **TigerBeetle VOPR** — deterministic simulator + strict state-machine
  design + heavy assertions; excellent talks/docs by Joran Dirk Greef.
- **rr** — process-level record/replay; retired-conditional-branches counter;
  chaos scheduling. Vocabulary for determinism engineering.
- **Microsoft Coyote / dBug / PCT** — systematic interleaving exploration
  strategies (candidate for Cosmos exploration v2+).
- **Jepsen** — the catalog of distributed-systems bug shapes; informs our
  fault taxonomy and property catalogs.
