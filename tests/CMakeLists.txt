# Use an installed GoogleTest when available (e.g. libgtest-dev in Docker),
# otherwise download it at configure time.
find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz)
  FetchContent_MakeAvailable(googletest)
endif()

add_executable(miniexchange_tests
  test_types.cpp
  test_order.cpp
  test_object_pool.cpp
  test_price_level.cpp
  test_order_book.cpp
  test_matching_engine.cpp
  test_reference_model.cpp
  test_trade_manager.cpp
  test_metrics.cpp
  test_persistence.cpp
  test_exchange.cpp
  test_concurrency.cpp
  test_cli.cpp)

target_link_libraries(miniexchange_tests PRIVATE mx::exchange mx::cli GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(miniexchange_tests DISCOVERY_TIMEOUT 120)
