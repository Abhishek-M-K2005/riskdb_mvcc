cmake_minimum_required(VERSION 3.14)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

project(ParallelMVCCEngine CXX)

# Require modern C++ for std::atomic and memory models
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Cross-platform aggressive compiler optimization
if(MSVC)
  add_compile_options(/O2 /W4)
else()
  add_compile_options(-O3 -march=native -Wall -Wextra)
endif()

# macOS Homebrew libomp auto-detection (Linux & Windows detect OpenMP natively)
if(APPLE)
  if(EXISTS "/opt/homebrew/opt/libomp")
    set(OpenMP_C_FLAGS "-Xpreprocessor -fopenmp -I/opt/homebrew/opt/libomp/include")
    set(OpenMP_C_LIB_NAMES "omp")
    set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp -I/opt/homebrew/opt/libomp/include")
    set(OpenMP_CXX_LIB_NAMES "omp")
    set(OpenMP_omp_LIBRARY "/opt/homebrew/opt/libomp/lib/libomp.dylib")
  elseif(EXISTS "/usr/local/opt/libomp")
    set(OpenMP_C_FLAGS "-Xpreprocessor -fopenmp -I/usr/local/opt/libomp/include")
    set(OpenMP_C_LIB_NAMES "omp")
    set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp -I/usr/local/opt/libomp/include")
    set(OpenMP_CXX_LIB_NAMES "omp")
    set(OpenMP_omp_LIBRARY "/usr/local/opt/libomp/lib/libomp.dylib")
  endif()
endif()

# Find OpenMP (Real GCC handles this natively)
find_package(OpenMP REQUIRED)

# Add system package search paths (macOS Homebrew, Linux /usr, Windows)
list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew" "/usr/local" "/usr")

# Find Google Test (prefer local system installation, fallback to shallow git clone)
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(googletest)
endif()

if(TARGET GTest::gtest_main AND NOT TARGET gtest_main)
  add_library(gtest_main ALIAS GTest::gtest_main)
endif()

# Find Google Benchmark (prefer local system installation, fallback to shallow git clone)
find_package(benchmark QUIET)
if(NOT benchmark_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.3
    GIT_SHALLOW TRUE
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googlebenchmark)
endif()

# Include directories
include_directories(include)

# Main Executable
add_executable(mvcc_engine src/main.cpp)
target_link_libraries(mvcc_engine PRIVATE OpenMP::OpenMP_CXX)

# Testing Executable
add_executable(mvcc_tests tests/test_engine.cpp)
target_link_libraries(mvcc_tests PRIVATE gtest_main OpenMP::OpenMP_CXX)

# Benchmark Executable
add_executable(mvcc_bench benchmarks/bench_ycsb.cpp)
target_link_libraries(mvcc_bench PRIVATE benchmark::benchmark benchmark::benchmark_main OpenMP::OpenMP_CXX)
