cmake_minimum_required(VERSION 3.16)
project(pulsar_benchmark LANGUAGES C CXX)

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

find_package(Threads REQUIRED)
# The `init` subcommands drive the Pulsar admin REST API directly via libcurl.
find_package(CURL REQUIRED)

# ----------------------------------------------------------------------------
# pulsar-client-cpp: prefer an installed library, else FetchContent a pinned
# release tag and build the static library from source. The source build
# needs protobuf (+protoc), openssl, curl, zlib, zstd, snappy and the boost
# headers on the host — all provided by `nix develop .#benchmarks` locally.
# ----------------------------------------------------------------------------
find_library(PULSAR_LIBRARY NAMES pulsar)
find_path(PULSAR_INCLUDE_DIR NAMES pulsar/Client.h)

add_library(pulsar_client INTERFACE)

if(PULSAR_LIBRARY AND PULSAR_INCLUDE_DIR)
  message(STATUS "Using system pulsar-client-cpp: ${PULSAR_LIBRARY}")
  target_include_directories(pulsar_client SYSTEM INTERFACE ${PULSAR_INCLUDE_DIR})
  target_link_libraries(pulsar_client INTERFACE ${PULSAR_LIBRARY})
else()
  message(STATUS "System pulsar-client-cpp not found; building from source")
  include(FetchContent)

  set(PULSAR_CLIENT_CPP_TAG "v4.2.0" CACHE STRING
      "pulsar-client-cpp git tag to build against")

  # Only the static library, no tests/tools.
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_PERF_TOOLS OFF CACHE BOOL "" FORCE)
  set(BUILD_DYNAMIC_LIB OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)

  FetchContent_Declare(
    pulsar-client-cpp
    GIT_REPOSITORY https://github.com/apache/pulsar-client-cpp.git
    GIT_TAG ${PULSAR_CLIENT_CPP_TAG}
  )
  FetchContent_MakeAvailable(pulsar-client-cpp)

  # pulsar's CMake does not declare PUBLIC includes on pulsarStatic when
  # consumed via FetchContent/add_subdirectory, so propagate them ourselves
  # (SYSTEM keeps our -Wextra/-Wpedantic from flagging the vendored headers).
  target_include_directories(pulsar_client SYSTEM INTERFACE
    ${pulsar-client-cpp_SOURCE_DIR}/include)
  target_link_libraries(pulsar_client INTERFACE pulsarStatic)

  # pulsar-client-cpp compiles with -Werror; our directory-wide
  # -Wextra/-Wpedantic surface warnings in its sources that would be
  # upgraded to errors. Demote them per-target instead of patching the
  # vendored source (same approach as valkey/CMakeLists.txt).
  foreach(_t PULSAR_OBJECT_LIB pulsarStatic pulsarShared pulsarShared_noh)
    if(TARGET ${_t})
      target_compile_options(${_t} PRIVATE -Wno-error)
    endif()
  endforeach()
endif()

# ----------------------------------------------------------------------------
# Shared object: pulsar transport + monotonic clock + arg helpers
# ----------------------------------------------------------------------------
add_library(pulsar_bench_core STATIC
  src/pulsar_transport.cpp
)
target_include_directories(pulsar_bench_core PUBLIC src)
target_link_libraries(pulsar_bench_core
  PUBLIC pulsar_client CURL::libcurl Threads::Threads ${CMAKE_DL_LIBS}
)

# ----------------------------------------------------------------------------
# Binaries
# ----------------------------------------------------------------------------
add_executable(rtt src/rtt.cpp)
target_link_libraries(rtt PRIVATE pulsar_bench_core)

add_executable(throughput src/throughput.cpp)
target_link_libraries(throughput PRIVATE pulsar_bench_core)

# ----------------------------------------------------------------------------
# Tests (shell-based, CTest-registered). Integration tests start a Pulsar
# standalone broker in docker on ports 26650/28080 (override via PULSAR_PORT /
# PULSAR_ADMIN_PORT) or reuse one that is already healthy.
# ----------------------------------------------------------------------------
enable_testing()

add_test(NAME cli_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/cli_test.sh)
set_tests_properties(cli_test PROPERTIES
  ENVIRONMENT "BUILD_DIR=${CMAKE_BINARY_DIR}" TIMEOUT 60)

# Failure path: a failed async flush() barrier must abort the produce run
# (nonzero exit, no CSV). Owns its broker (ports 26651/28081, override via
# PULSAR_FAILURE_PORT / PULSAR_FAILURE_ADMIN_PORT) because it kills it.
add_test(NAME throughput_flush_failure
         COMMAND bash
                 ${CMAKE_CURRENT_SOURCE_DIR}/test/throughput_flush_failure.sh)
set_tests_properties(throughput_flush_failure PROPERTIES
  ENVIRONMENT "BUILD_DIR=${CMAKE_BINARY_DIR}" TIMEOUT 600)

foreach(transport persistent non-persistent)
  foreach(mode sync async)
    add_test(NAME rtt_integration_${transport}_${mode}
             COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/test/rtt_integration.sh
                     ${transport} ${mode})
    set_tests_properties(rtt_integration_${transport}_${mode} PROPERTIES
      ENVIRONMENT "BUILD_DIR=${CMAKE_BINARY_DIR}" TIMEOUT 600)

    add_test(NAME throughput_integration_${transport}_${mode}
             COMMAND bash
                     ${CMAKE_CURRENT_SOURCE_DIR}/test/throughput_integration.sh
                     ${transport} ${mode})
    set_tests_properties(throughput_integration_${transport}_${mode} PROPERTIES
      ENVIRONMENT "BUILD_DIR=${CMAKE_BINARY_DIR}" TIMEOUT 600)
  endforeach()
endforeach()
