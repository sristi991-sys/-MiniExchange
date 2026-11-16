cmake_minimum_required(VERSION 3.20)

project(MiniExchange
  VERSION 1.0.0
  DESCRIPTION "High-performance stock exchange and order matching engine"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language standard: C++20 by default, C++17 supported.
# ---------------------------------------------------------------------------
set(MX_CXX_STANDARD 20 CACHE STRING "C++ standard to compile with (17 or 20)")
set_property(CACHE MX_CXX_STANDARD PROPERTY STRINGS 17 20)
set(CMAKE_CXX_STANDARD ${MX_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MX_BUILD_TESTS "Build the Google Test suite" ON)
option(MX_BUILD_BENCHMARKS "Build the benchmark executable" ON)
option(MX_BUILD_APP "Build the interactive exchange CLI" ON)
option(MX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MX_ENABLE_ASAN "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MX_ENABLE_TSAN "Build with ThreadSanitizer" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CompilerWarnings)
include(Sanitizers)

find_package(Threads REQUIRED)

if(MINGW)
  # Link the C++ runtime statically so the binaries run without MinGW DLLs on PATH.
  add_link_options(-static)
endif()

add_subdirectory(src)
add_subdirectory(app)

if(MX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MX_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

message(STATUS "MiniExchange ${PROJECT_VERSION}: C++${CMAKE_CXX_STANDARD}, ${CMAKE_BUILD_TYPE} build")
