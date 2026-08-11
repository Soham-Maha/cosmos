# Justfile for Cosmos

set shell := ["bash", "-uc"]

default:
    @just --list

# Build production binaries
build:
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --parallel --target kv_store_prod replicated_kv_prod

# Build and run unit tests
test:
    cmake -B build -DCOSMOS_BUILD_TESTS=ON
    cmake --build build --parallel
    ctest --test-dir build --output-on-failure

# Build all targets (production binaries, simulation examples, and tests)
all:
    cmake -B build -DCOSMOS_BUILD_TESTS=ON
    cmake --build build --parallel

# Format all C/C++ source and header files
format:
    find include src tests examples -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" -o -name "*.h" \) -exec clang-format -i {} +

# Check code formatting without modifying files
format-check:
    find include src tests examples -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" -o -name "*.h" \) -exec clang-format --dry-run --Werror {} +

# Remove build directory
clean:
    rm -rf build

alias b := build
alias t := test
alias fmt := format
alias c := clean
