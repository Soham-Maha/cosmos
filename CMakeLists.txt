cmake_minimum_required(VERSION 3.25)
project(cosmos VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(COSMOS_BUILD_TESTS "Build Cosmos unit tests" ON)

add_subdirectory(src/cosmos)
add_subdirectory(examples)

if(COSMOS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
