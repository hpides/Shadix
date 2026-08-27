cmake_minimum_required(VERSION 3.16)
project(nats_benchmark LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)
# Match the Rust profile: target-cpu=native.
add_compile_options(-march=native)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3)
endif()

# ----------------------------------------------------------------------------
# nats.c via FetchContent
# Static lib, no TLS, no NATS Streaming, no examples/tests.
# ----------------------------------------------------------------------------
include(FetchContent)
find_package(Threads REQUIRED)

set(NATS_C_TAG "v3.13.0" CACHE STRING "nats.c git tag to build against")

set(NATS_BUILD_WITH_TLS OFF CACHE BOOL "" FORCE)
set(NATS_BUILD_STREAMING OFF CACHE BOOL "" FORCE)
set(NATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NATS_BUILD_LIB_SHARED OFF CACHE BOOL "" FORCE)
set(NATS_BUILD_LIB_STATIC ON CACHE BOOL "" FORCE)
# nats.c registers its own 300+ CTest cases when BUILD_TESTING is on; keep the
# suite to our shell tests only (enable_testing() below is unaffected).
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  natsc
  GIT_REPOSITORY https://github.com/nats-io/nats.c.git
  GIT_TAG ${NATS_C_TAG}
)
FetchContent_MakeAvailable(natsc)

# ----------------------------------------------------------------------------
# Shared object: transport layer + monotonic clock
# ----------------------------------------------------------------------------
add_library(natsbench_core STATIC
  src/nats_transport.cpp
)
target_include_directories(natsbench_core
  PUBLIC src
         "${natsc_SOURCE_DIR}/src"
)
target_link_libraries(natsbench_core
  PUBLIC nats_static Threads::Threads
)

# ----------------------------------------------------------------------------
# Binaries
# ----------------------------------------------------------------------------
add_executable(rtt src/rtt.cpp)
target_link_libraries(rtt PRIVATE natsbench_core)

add_executable(throughput src/throughput.cpp)
target_link_libraries(throughput PRIVATE natsbench_core)

# ----------------------------------------------------------------------------
# Tests (shell scripts driven by CTest)
# ----------------------------------------------------------------------------
enable_testing()

add_test(NAME cli_test
  COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/cli_test.sh
          $<TARGET_FILE:rtt> $<TARGET_FILE:throughput>)
set_tests_properties(cli_test PROPERTIES TIMEOUT 60)

# Each integration test starts its own nats-server, so each gets a unique
# port (24223..24232) to keep `ctest -j` safe: with a shared port the fixture
# readiness poll would accept another test's server and the tests would
# cross-consume each other's messages. Port 24222 stays the default for
# running the scripts by hand.
set(_test_port 24223)
foreach(_transport core js)
  foreach(_mode sync async)
    add_test(NAME rtt_integration_${_transport}_${_mode}
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/rtt_integration.sh
              ${_transport} ${_mode} $<TARGET_FILE:rtt>)
    set_tests_properties(rtt_integration_${_transport}_${_mode} PROPERTIES
      TIMEOUT 120
      ENVIRONMENT "NATS_TEST_PORT=${_test_port}")
    math(EXPR _test_port "${_test_port} + 1")

    add_test(NAME throughput_integration_${_transport}_${_mode}
      COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/throughput_integration.sh
              ${_transport} ${_mode} $<TARGET_FILE:throughput>)
    set_tests_properties(throughput_integration_${_transport}_${_mode}
      PROPERTIES
      TIMEOUT 120
      ENVIRONMENT "NATS_TEST_PORT=${_test_port}")
    math(EXPR _test_port "${_test_port} + 1")
  endforeach()

  # Failure path: a dead broker must fail the async flush barrier (exit 1,
  # no CSV) instead of hanging or reporting a complete run. This test kills
  # its own nats-server mid-run.
  add_test(NAME throughput_flush_failure_${_transport}
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/throughput_flush_failure.sh
            ${_transport} $<TARGET_FILE:throughput>)
  set_tests_properties(throughput_flush_failure_${_transport} PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "NATS_TEST_PORT=${_test_port}")
  math(EXPR _test_port "${_test_port} + 1")
endforeach()
