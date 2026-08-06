cmake_minimum_required(VERSION 3.16)
project(fanorona_engine CXX)

# std::popcount / std::countr_zero, which lower to single instructions on both
# x86 and WebAssembly.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(fanorona_core STATIC
    src/board.cpp
    src/zobrist.cpp
    src/eval.cpp
    src/search.cpp
    src/codec.cpp
)
target_include_directories(fanorona_core PUBLIC src)
target_link_libraries(fanorona_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(fanorona_core PRIVATE /O2)
else()
    target_compile_options(fanorona_core PRIVATE -O3)
endif()

# Deliberately no -march=native: the AVX-512 evaluator is selected at runtime
# via __builtin_cpu_supports, so one binary runs everywhere and still uses the
# wide path on hardware that has it. Baking in -march would trade that for a
# binary that crashes on older CPUs.

add_executable(fanorona_bench src/bench.cpp)
target_link_libraries(fanorona_bench PRIVATE fanorona_core)

add_executable(fanorona src/server.cpp src/trashtalk.cpp)
target_link_libraries(fanorona PRIVATE fanorona_core)
if(WIN32)
    target_link_libraries(fanorona PRIVATE ws2_32)
endif()
