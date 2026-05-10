# CMakeLists.txt ────────────────────────────────────────────────────────────
# C++17 because we use std::optional, std::from_chars, structured bindings.
# Threads::Threads links pthreads on Linux/macOS.

cmake_minimum_required(VERSION 3.14)
project(request_simulator CXX)

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_executable(request_sim
    src/main.cpp
    src/DatasetLoader.cpp
    src/TrafficEngine.cpp
    src/RequestSimulator.cpp
    src/SimConfig.cpp
)

target_include_directories(request_sim PRIVATE include)
target_link_libraries(request_sim PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(request_sim PRIVATE /W4 /permissive-)
else()
    target_compile_options(request_sim PRIVATE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
endif()
