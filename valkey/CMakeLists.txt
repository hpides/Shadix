cmake_minimum_required(VERSION 3.16)
project(valkey_rdma_benchmark LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
# Match the Rust profile: target-cpu=native, LTO, no exceptions on the hot path.
add_compile_options(-march=native)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3)
endif()

# ----------------------------------------------------------------------------
# libvalkey via FetchContent
# Build with RDMA support; dlopen so the binary still loads on hosts without
# librdmacm/libibverbs (RDMA connects will fail at runtime instead of link time).
# ----------------------------------------------------------------------------
include(FetchContent)
include(CheckIncludeFile)
find_package(Threads REQUIRED)

set(LIBVALKEY_TAG "0.4.0" CACHE STRING "libvalkey git tag to build against")
# NOTE: must match the libvalkey version vendored under deps/libvalkey/ in the
# valkey-server you're talking to. Valkey 9.1.0 ships libvalkey 0.4.0; the
# RDMA wire protocol is experimental and changed between 0.4.0 and 0.5.0,
# producing a segfault on the first write if the versions don't match.

# RDMA requires rdma/rdma_cma.h at libvalkey build time (even with
# DLOPEN_RDMA the headers are included for type definitions). Auto-disable
# on hosts without rdma-core headers so the TCP-only build still works.
check_include_file("rdma/rdma_cma.h" HAVE_RDMA_HEADERS)
option(BUILD_RDMA "Build with libvalkey RDMA transport" ${HAVE_RDMA_HEADERS})

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_TLS OFF CACHE BOOL "" FORCE)
set(DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
if(BUILD_RDMA)
  set(ENABLE_RDMA ON CACHE BOOL "" FORCE)
  set(ENABLE_DLOPEN_RDMA ON CACHE BOOL "" FORCE)
else()
  set(ENABLE_RDMA OFF CACHE BOOL "" FORCE)
  message(STATUS "RDMA disabled (rdma/rdma_cma.h not found). "
                 "Binaries will reject rdma:// URLs at runtime.")
endif()
set(ENABLE_THREADS ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  libvalkey
  GIT_REPOSITORY https://github.com/valkey-io/libvalkey.git
  GIT_TAG ${LIBVALKEY_TAG}
)
FetchContent_MakeAvailable(libvalkey)

# libvalkey ships -Werror in its CMake; some unused-variable warnings in
# 0.5.0's rdma.c are upgraded to errors on older gcc. Demote them per-target
# so we don't have to patch the vendored source.
foreach(_t valkey valkey_rdma)
  if(TARGET ${_t})
    target_compile_options(${_t} PRIVATE -Wno-error)
  endif()
endforeach()

# ----------------------------------------------------------------------------
# Shared object: connection + list transport + monotonic clock
# ----------------------------------------------------------------------------
add_library(vrdma_bench_core STATIC
  src/connection.cpp
  src/list_transport.cpp
)
target_include_directories(vrdma_bench_core
  PUBLIC src
         # libvalkey's CMake doesn't declare PUBLIC includes on its targets
         # when consumed via FetchContent/add_subdirectory, so we propagate
         # them ourselves.
         "${libvalkey_SOURCE_DIR}/include"
)
target_link_libraries(vrdma_bench_core
  PUBLIC valkey::valkey Threads::Threads ${CMAKE_DL_LIBS}
)
if(BUILD_RDMA)
  target_link_libraries(vrdma_bench_core PUBLIC valkey::valkey_rdma)
  target_compile_definitions(vrdma_bench_core PUBLIC VRDMA_HAVE_RDMA=1)
endif()

# ----------------------------------------------------------------------------
# Binaries
# ----------------------------------------------------------------------------
add_executable(rtt src/rtt.cpp)
target_link_libraries(rtt PRIVATE vrdma_bench_core)

add_executable(throughput src/throughput.cpp)
target_link_libraries(throughput PRIVATE vrdma_bench_core)

# ----------------------------------------------------------------------------
# Tests (shell scripts driven by CTest)
# ----------------------------------------------------------------------------
enable_testing()

add_test(NAME cli_test
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/cli_test.sh
          $<TARGET_FILE:rtt> $<TARGET_FILE:throughput>)
set_tests_properties(cli_test PROPERTIES TIMEOUT 60)

# Each integration test starts its own Valkey container (docker), so each
# gets a unique host port (26380..26384) to keep `ctest -j` safe: with a
# shared port the fixture would refuse to start (or, worse, tests would
# cross-consume each other's lists). Port 26379 stays the default for
# running the scripts by hand. The generous timeout covers a first-time
# image pull.
set(_test_port 26380)
foreach(_mode sync async)
  add_test(NAME rtt_integration_list_${_mode}
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/rtt_integration.sh
            ${_mode} $<TARGET_FILE:rtt>)
  set_tests_properties(rtt_integration_list_${_mode} PROPERTIES
    TIMEOUT 300
    ENVIRONMENT "VALKEY_TEST_PORT=${_test_port}")
  math(EXPR _test_port "${_test_port} + 1")

  add_test(NAME throughput_integration_list_${_mode}
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/throughput_integration.sh
            ${_mode} $<TARGET_FILE:throughput>)
  set_tests_properties(throughput_integration_list_${_mode} PROPERTIES
    TIMEOUT 300
    ENVIRONMENT "VALKEY_TEST_PORT=${_test_port}")
  math(EXPR _test_port "${_test_port} + 1")
endforeach()

# Failure path: a dead broker must fail the async produce run (exit 1, no
# CSV) instead of hanging or reporting a complete run. This test removes its
# own container mid-run.
add_test(NAME throughput_flush_failure_list
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/throughput_flush_failure.sh
          $<TARGET_FILE:throughput>)
set_tests_properties(throughput_flush_failure_list PROPERTIES
  TIMEOUT 300
  ENVIRONMENT "VALKEY_TEST_PORT=${_test_port}")
