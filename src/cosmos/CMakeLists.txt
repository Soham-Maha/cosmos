add_library(cosmos STATIC
    cosmos.cpp
)

target_include_directories(cosmos PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
