# Cosmos

Cosmos is an embeddable C++ library for **Deterministic Simulation Testing (DST)** for C/C++ applications via standard POSIX library function interposition (`-Wl,--wrap`).

It provides zero-code-change simulation testing for standard POSIX functions (`malloc`, `free`, `pthread_create`, `clock_gettime`, `socket`, `send`, `recv`, `open`, `write`, `fsync`, `getrandom`).

## Documentation

Full documentation is available in the [`docs/`](docs/) directory:

- [Project Plan](docs/plan.md) – Goals, architecture decisions, POSIX interposition taxonomy, and roadmap.
- [Architecture](docs/architecture.md) – Build-flag interposition layer, substrate seams, and engine details.
- [Design Specification](docs/design.md) – Authoritative public API reference and subsystem designs.
- [Antithesis Study Notes](docs/antithesis-study-notes.md) – DST research and background.

## Quick Start

```bash
# Configure and build libcosmos and examples
cmake -B build
cmake --build build
```

## Examples

- [`examples/single_node/`](examples/single_node/) – Transactional WAL storage engine (crash durability & OOM fault injection).
- [`examples/distributed/`](examples/distributed/) – Replicated consensus cluster (network partitions & message reordering).
