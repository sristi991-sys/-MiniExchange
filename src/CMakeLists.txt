# Each module is its own static library with explicit dependencies, which keeps
# the layering honest: lower layers can never include higher ones.
#
#   core <- orderbook <- matching <-+
#   core <- trade ------------------+- exchange
#   core <- persistence ------------+
#   core <- metrics ----------------+

function(mx_add_module name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS" ${ARGN})
  add_library(mx_${name} STATIC ${ARG_SOURCES})
  add_library(mx::${name} ALIAS mx_${name})
  target_include_directories(mx_${name} PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
  target_link_libraries(mx_${name} PUBLIC ${ARG_DEPS})
  target_compile_features(mx_${name} PUBLIC cxx_std_17)
  mx_set_warnings(mx_${name})
endfunction()

mx_add_module(core
  SOURCES core/Types.cpp core/Order.cpp core/Trade.cpp core/ExecutionReport.cpp
  DEPS Threads::Threads)

mx_add_module(metrics
  SOURCES metrics/LatencyHistogram.cpp metrics/PerformanceMonitor.cpp
  DEPS mx::core)

mx_add_module(orderbook
  SOURCES orderbook/OrderBook.cpp
  DEPS mx::core)

mx_add_module(matching
  SOURCES matching/MatchingEngine.cpp
  DEPS mx::orderbook)

mx_add_module(trade
  SOURCES trade/TradeManager.cpp
  DEPS mx::core)

mx_add_module(persistence
  SOURCES persistence/EventJournal.cpp persistence/TradeStore.cpp
  DEPS mx::core)

mx_add_module(exchange
  SOURCES exchange/Exchange.cpp exchange/OrderGateway.cpp
  DEPS mx::matching mx::trade mx::persistence mx::metrics)
