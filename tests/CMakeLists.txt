# Tests assert() their expectations, so NDEBUG would silently compile every check away and
# report a vacuous pass. CMake caches CMAKE_BUILD_TYPE across `just build` and `just test`,
# so a prior Release configure would otherwise leak NDEBUG into the test binaries.
add_compile_options(-UNDEBUG)

add_executable(test_wrap_malloc test_wrap_malloc.cpp)
target_link_libraries(test_wrap_malloc PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(test_wrap_malloc PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
    )
endif()

add_test(NAME test_wrap_malloc COMMAND test_wrap_malloc)

add_executable(test_random test_random.cpp)
target_link_libraries(test_random PRIVATE cosmos)
add_test(NAME test_random COMMAND test_random)

add_executable(test_seed_derivation test_seed_derivation.cpp)
target_link_libraries(test_seed_derivation PRIVATE cosmos)
add_test(NAME test_seed_derivation COMMAND test_seed_derivation)
