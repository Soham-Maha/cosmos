.PHONY: default build test all format format-check clean

default:
	@echo "Cosmos build tasks:"
	@echo "  make build         - Build production binaries (Release)"
	@echo "  make test          - Build and run unit tests"
	@echo "  make all           - Build production binaries, examples, and tests"
	@echo "  make format        - Format code with clang-format"
	@echo "  make format-check  - Verify code formatting without modifying files"
	@echo "  make clean         - Remove build directory"

build:
	cmake -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build --parallel --target kv_store_prod replicated_kv_prod

test:
	cmake -B build -DCOSMOS_BUILD_TESTS=ON
	cmake --build build --parallel
	ctest --test-dir build --output-on-failure

all:
	cmake -B build -DCOSMOS_BUILD_TESTS=ON
	cmake --build build --parallel

format:
	find include src tests examples -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" -o -name "*.h" \) -exec clang-format -i {} +

format-check:
	find include src tests examples -type f \( -name "*.cpp" -o -name "*.hpp" -o -name "*.c" -o -name "*.h" \) -exec clang-format --dry-run --Werror {} +

clean:
	rm -rf build
