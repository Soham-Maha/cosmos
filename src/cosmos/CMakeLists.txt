add_library(cosmos STATIC
    cosmos.cpp
    wrappers/wrap_memory.cpp
    wrappers/wrap_threads.cpp
    wrappers/wrap_time.cpp
    wrappers/wrap_net.cpp
    wrappers/wrap_storage.cpp
    wrappers/wrap_random.cpp
)

target_include_directories(cosmos PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
