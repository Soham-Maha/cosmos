# Cosmos

Cosmos is an embeddable C++ library for **Deterministic Simulation Testing (DST)** for C/C++ applications via standard POSIX library function interposition (`-Wl,--wrap`).

It provides zero-code-change simulation testing for standard POSIX functions (`malloc`, `free`, `pthread_create`, `clock_gettime`, `socket`, `send`, `recv`, `open`, `write`, `fsync`, `getrandom`).

## Documentation

Full documentation is available in the [`docs/`](docs/) directory:

- [Project Plan](docs/plan.md) – Goals, architecture decisions, POSIX interposition taxonomy, and roadmap.
- [Architecture](docs/architecture.md) – Build-flag interposition layer, substrate seams, and engine details.
- [Design Specification](docs/design.md) – Authoritative public API reference and subsystem designs.
- [Linker Interposition](docs/linker-interposition.md) – POSIX wrapper interposition mechanics and linker symbol resolution.
- [Antithesis Study Notes](docs/antithesis-study-notes.md) – DST research and background.

## Quick Start

Using `just` or `make`:

```bash
# Production build
just build       # or make build

# Build and run tests
just test        # or make test

# Build all targets (production binaries, simulation examples, and tests)
just all         # or make all

# Format codebase & check formatting
just format      # or make format
just format-check # or make format-check
just lint        # or make lint


# Clean build artifacts
just clean       # or make clean
```

Or using CMake directly:

```bash
# Configure and build libcosmos, tests, and simulation examples
cmake -B build -DCOSMOS_BUILD_TESTS=ON
cmake --build build

# Run unit tests
ctest --test-dir build --output-on-failure

# Run simulation examples directly
./build/examples/single_node/kv_store_sim
./build/examples/distributed/replicated_kv_sim
```

## Examples

- [`examples/single_node/`](examples/single_node/) – Transactional WAL storage engine (crash durability & OOM fault injection).
- [`examples/distributed/`](examples/distributed/) – Replicated consensus cluster (network partitions & message reordering).
