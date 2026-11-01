cmake_minimum_required(VERSION 3.14)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

project(ParallelMVCCEngine CXX)

# Require modern C++ for std::atomic and memory models
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Enable aggressive optimization for the engine
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native")

# Support Homebrew libomp on macOS
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

# Fetch Google Test
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
FetchContent_MakeAvailable(googletest)

# Fetch Google Benchmark
FetchContent_Declare(
  googlebenchmark
  URL https://github.com/google/benchmark/archive/v1.8.3.zip
)
# Disable Google Benchmark's internal tests and strict warnings
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

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
