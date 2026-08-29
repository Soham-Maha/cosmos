add_executable(test_wrap_malloc test_wrap_malloc.cpp)
target_link_libraries(test_wrap_malloc PRIVATE cosmos)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    # Linker --wrap substitutes malloc/free AFTER optimization, but GCC/Clang
    # treat them as builtins with no observable side effects. Under -O2 the
    # optimizers cache/reorder loads of sim-heap bookkeeping across the wrapped
    # calls, and clang folds out-of-bounds header reads as UB. -fno-builtin
    # makes the wrapped symbols opaque calls again. See docs/linker-interposition.md.
    target_compile_options(test_wrap_malloc PRIVATE
        "-fno-builtin-malloc"
        "-fno-builtin-free"
    )
    target_link_options(test_wrap_malloc PRIVATE
        "-Wl,--gc-sections"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=free"
    )
endif()

add_test(NAME test_wrap_malloc COMMAND test_wrap_malloc)
