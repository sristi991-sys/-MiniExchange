// MiniExchange performance benchmarks.
//
// Self-contained harness (no external benchmark library). Every scenario runs a
// pre-generated, seeded workload so results are reproducible and generation
// cost is excluded from the timings.
//
//   miniexchange_bench [--ops N] [--seed S] [--threads 1,2,4,8] [--quick] [--markdown]

#include "mx/exchange/Exchange.hpp"
#include "mx/exchange/OrderGateway.hpp"
#include "mx/matching/MatchingEngine.hpp"
#include "mx/metrics/LatencyHistogram.hpp"
#include "mx/util/Clock.hpp"
#include "mx/util/ObjectPool.hpp"
#include "mx/util/StringUtils.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace mx;

// ---------------------------------------------------------------------------
// Workload
// ---------------------------------------------------------------------------

struct Operation {
  enum class Kind : std::uint8_t { Limit, Market, Cancel };
  Kind kind{Kind::Limit};
  Side side{Side::Buy};
  TimeInForce tif{TimeInForce::GTC};
  Price price{0};
  Quantity quantity{0};
  std::size_t target{0};  ///< Cancel: index into the caller's list of its own order ids.
};

/// Realistic-ish order flow around a mid price of 100.00:
///   70% limit orders (90% GTC, 10% IOC) priced mid +/- 0.25 so roughly half cross,
///    8% market orders, 22% cancels of one of the last 2000 orders.
std::vector<Operation> generateWorkload(std::size_t count, std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<int> percent(0, 99);
  std::uniform_int_distribution<int> offset(-25, 25);
  std::uniform_int_distribution<Quantity> quantity(1, 100);
  constexpr Price kMid = 100 * kPriceScale;

  std::vector<Operation> ops;
  ops.reserve(count);
  std::size_t ordersSoFar = 0;
  for (std::size_t i = 0; i < count; ++i) {
    Operation op;
    const int roll = percent(rng);
    op.side = percent(rng) < 50 ? Side::Buy : Side::Sell;
    if (roll < 22 && ordersSoFar > 0) {
      op.kind = Operation::Kind::Cancel;
      const std::size_t window = std::min<std::size_t>(ordersSoFar, 2000);
      op.target = ordersSoFar - 1 - std::uniform_int_distribution<std::size_t>(0, window - 1)(rng);
    } else if (roll < 30) {
      op.kind = Operation::Kind::Market;
      op.tif = TimeInForce::IOC;
      op.quantity = quantity(rng);
      ++ordersSoFar;
    } else {
      op.kind = Operation::Kind::Limit;
      op.tif = percent(rng) < 90 ? TimeInForce::GTC : TimeInForce::IOC;
      op.price = kMid + offset(rng);
      op.quantity = quantity(rng);
      ++ordersSoFar;
    }
    ops.push_back(op);
  }
  return ops;
}

OrderRequest toRequest(const Operation& op, const std::string& symbol) {
  return op.kind == Operation::Kind::Market
             ? OrderRequest::market(symbol, op.side, op.quantity)
             : OrderRequest::limit(symbol, op.side, op.quantity, op.price, op.tif);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

struct Result {
  std::string name;
  std::uint64_t operations{0};
  double seconds{0.0};
  std::uint64_t trades{0};
  std::optional<LatencySummary> latency;

  [[nodiscard]] double opsPerSecond() const {
    return seconds > 0 ? static_cast<double>(operations) / seconds : 0.0;
  }
};

std::string humanRate(double perSecond) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2);
  if (perSecond >= 1e6) {
    out << perSecond / 1e6 << " M/s";
  } else if (perSecond >= 1e3) {
    out << perSecond / 1e3 << " K/s";
  } else {
    out << perSecond << " /s";
  }
  return out.str();
}

void printTable(const std::vector<Result>& results, bool markdown) {
  if (markdown) {
    std::cout << "\n| Benchmark | Ops | Time (s) | Throughput | Trades | p50 (ns) | p99 (ns) | "
                 "p99.9 (ns) |\n|---|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const Result& r : results) {
      std::cout << "| " << r.name << " | " << r.operations << " | " << std::fixed
                << std::setprecision(3) << r.seconds << " | " << humanRate(r.opsPerSecond())
                << " | " << r.trades << " | ";
      if (r.latency) {
        std::cout << r.latency->p50 << " | " << r.latency->p99 << " | " << r.latency->p999
                  << " |\n";
      } else {
        std::cout << "- | - | - |\n";
      }
    }
    return;
  }
  std::cout << '\n'
            << std::left << std::setw(46) << "Benchmark" << std::right << std::setw(10) << "Ops"
            << std::setw(10) << "Time(s)" << std::setw(13) << "Throughput" << std::setw(10)
            << "Trades" << std::setw(9) << "p50ns" << std::setw(9) << "p99ns" << std::setw(10)
            << "p99.9ns" << '\n'
            << std::string(117, '-') << '\n';
  for (const Result& r : results) {
    std::cout << std::left << std::setw(46) << r.name << std::right << std::setw(10)
              << r.operations << std::setw(10) << std::fixed << std::setprecision(3) << r.seconds
              << std::setw(13) << humanRate(r.opsPerSecond()) << std::setw(10) << r.trades;
    if (r.latency) {
      std::cout << std::setw(9) << r.latency->p50 << std::setw(9) << r.latency->p99
                << std::setw(10) << r.latency->p999;
    } else {
      std::cout << std::setw(9) << "-" << std::setw(9) << "-" << std::setw(10) << "-";
    }
    std::cout << '\n';
  }
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

/// Raw matching engine, single thread, no locks. With kMeasureLatency every
/// operation is timed individually (adds ~2 clock reads per op).
template <bool kMeasureLatency>
Result benchEngine(const std::vector<Operation>& ops) {
  MatchingEngine engine("BENCH");
  engine.reserve(16'384);
  LatencyHistogram histogram;
  std::vector<OrderId> ids;
  ids.reserve(ops.size());
  OrderId nextId = 1;
  std::uint64_t trades = 0;

  const std::uint64_t start = steadyNanos();
  for (const Operation& op : ops) {
    const std::uint64_t t0 = kMeasureLatency ? steadyNanos() : 0;
    if (op.kind == Operation::Kind::Cancel) {
      (void)engine.cancel(ids[op.target]);
    } else {
      const OrderType type =
          op.kind == Operation::Kind::Market ? OrderType::Market : OrderType::Limit;
      ids.push_back(nextId);
      const ExecutionReport report =
          engine.process(Order(nextId++, "BENCH", op.side, type, op.tif, op.price, op.quantity));
      trades += report.trades.size();
    }
    if constexpr (kMeasureLatency) {
      histogram.record(steadyNanos() - t0);
    }
  }
  const std::uint64_t elapsed = steadyNanos() - start;

  Result result;
  result.name = kMeasureLatency ? "MatchingEngine, 1 thread (per-op timing)"
                                : "MatchingEngine, 1 thread (throughput)";
  result.operations = ops.size();
  result.seconds = static_cast<double>(elapsed) / 1e9;
  result.trades = trades;
  if (kMeasureLatency) {
    result.latency = histogram.summary();
  }
  return result;
}

/// Full Exchange path (validation, locking, ledger, metrics) on one thread.
Result benchExchangeSingle(const std::vector<Operation>& ops) {
  ExchangeConfig config;
  config.storeTradeHistory = false;
  Exchange exchange(config);
  exchange.addSymbol("BENCH");
  std::vector<OrderId> ids;
  ids.reserve(ops.size());
  std::uint64_t trades = 0;

  const std::uint64_t start = steadyNanos();
  for (const Operation& op : ops) {
    if (op.kind == Operation::Kind::Cancel) {
      (void)exchange.cancelOrder("BENCH", ids[op.target]);
    } else {
      const ExecutionReport report = exchange.submitOrder(toRequest(op, "BENCH"));
      ids.push_back(report.orderId);
      trades += report.trades.size();
    }
  }
  const std::uint64_t elapsed = steadyNanos() - start;

  Result result;
  result.name = "Exchange, 1 thread (validation+locks+metrics)";
  result.operations = ops.size();
  result.seconds = static_cast<double>(elapsed) / 1e9;
  result.trades = trades;
  const MetricsSnapshot metrics = exchange.metrics().snapshot();
  result.latency = metrics.orderLatency;  // Measured by the exchange itself.
  return result;
}

/// T client threads hitting the Exchange concurrently. With `sharedSymbol`
/// they all trade one symbol (worst-case lock contention); otherwise each
/// thread owns a symbol (embarrassingly parallel).
Result benchExchangeThreads(std::size_t threads, std::size_t opsPerThread, std::uint64_t seed,
                            bool sharedSymbol) {
  ExchangeConfig config;
  config.storeTradeHistory = false;
  Exchange exchange(config);
  std::vector<std::string> symbols;
  for (std::size_t t = 0; t < threads; ++t) {
    symbols.push_back("SYM" + std::to_string(t));
    exchange.addSymbol(symbols.back());
  }
  std::vector<std::vector<Operation>> workloads;
  for (std::size_t t = 0; t < threads; ++t) {
    workloads.push_back(generateWorkload(opsPerThread, seed + t));
  }

  std::atomic<std::size_t> ready{0};
  std::atomic<bool> go{false};
  std::atomic<std::uint64_t> trades{0};
  std::vector<std::thread> pool;
  for (std::size_t t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      const std::string& symbol = sharedSymbol ? symbols.front() : symbols[t];
      std::vector<OrderId> ids;
      ids.reserve(opsPerThread);
      std::uint64_t localTrades = 0;
      ready.fetch_add(1);
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (const Operation& op : workloads[t]) {
        if (op.kind == Operation::Kind::Cancel) {
          (void)exchange.cancelOrder(symbol, ids[op.target]);
        } else {
          const ExecutionReport report = exchange.submitOrder(toRequest(op, symbol));
          ids.push_back(report.orderId);
          localTrades += report.trades.size();
        }
      }
      trades.fetch_add(localTrades);
    });
  }
  while (ready.load() < threads) {
    std::this_thread::yield();
  }
  const std::uint64_t start = steadyNanos();
  go.store(true, std::memory_order_release);
  for (auto& thread : pool) {
    thread.join();
  }
  const std::uint64_t elapsed = steadyNanos() - start;

  Result result;
  result.name = "Exchange, " + std::to_string(threads) + " threads, " +
                (sharedSymbol ? "1 shared symbol" : "1 symbol/thread");
  result.operations = threads * opsPerThread;
  result.seconds = static_cast<double>(elapsed) / 1e9;
  result.trades = trades.load();
  return result;
}

/// Async gateway: one producer, futures for every order, N worker shards.
Result benchGateway(std::size_t workers, std::size_t orders, std::uint64_t seed) {
  ExchangeConfig config;
  config.storeTradeHistory = false;
  Exchange exchange(config);
  constexpr std::size_t kSymbols = 16;
  std::vector<std::string> symbols;
  for (std::size_t s = 0; s < kSymbols; ++s) {
    symbols.push_back("G" + std::to_string(s));
    exchange.addSymbol(symbols.back());
  }
  std::vector<Operation> ops = generateWorkload(orders, seed);
  ops.erase(std::remove_if(ops.begin(), ops.end(),
                           [](const Operation& op) { return op.kind == Operation::Kind::Cancel; }),
            ops.end());

  OrderGateway gateway(exchange, workers);
  std::vector<std::future<ExecutionReport>> futures;
  futures.reserve(ops.size());

  const std::uint64_t start = steadyNanos();
  for (std::size_t i = 0; i < ops.size(); ++i) {
    futures.push_back(gateway.submit(toRequest(ops[i], symbols[i % kSymbols])));
  }
  std::uint64_t trades = 0;
  for (auto& future : futures) {
    trades += future.get().trades.size();
  }
  const std::uint64_t elapsed = steadyNanos() - start;

  Result result;
  result.name = "OrderGateway async, " + std::to_string(workers) + " workers, 16 symbols";
  result.operations = ops.size();
  result.seconds = static_cast<double>(elapsed) / 1e9;
  result.trades = trades;
  return result;
}

/// Allocation pattern of an order book: a sliding window of live nodes.
template <bool kUsePool>
Result benchAllocator(std::size_t operations) {
  const Order prototype(1, "BENCH", Side::Buy, OrderType::Limit, TimeInForce::GTC, 10000, 10);
  constexpr std::size_t kWindow = 4096;
  std::mt19937_64 rng(7);
  std::uniform_int_distribution<std::size_t> pick(0, kWindow - 1);

  ObjectPool<OrderNode> pool(4096);
  std::vector<ObjectPool<OrderNode>::Ptr> pooled(kWindow);
  std::vector<std::unique_ptr<OrderNode>> heap(kWindow);

  const std::uint64_t start = steadyNanos();
  for (std::size_t i = 0; i < operations; ++i) {
    const std::size_t slot = pick(rng);
    if constexpr (kUsePool) {
      pooled[slot] = pool.make(prototype);  // The replaced node goes back to the pool.
    } else {
      heap[slot] = std::make_unique<OrderNode>(prototype);
    }
  }
  const std::uint64_t elapsed = steadyNanos() - start;

  Result result;
  result.name = kUsePool ? "ObjectPool<OrderNode> alloc+free" : "new/delete OrderNode alloc+free";
  result.operations = operations;
  result.seconds = static_cast<double>(elapsed) / 1e9;
  return result;
}

struct Options {
  std::size_t ops = 1'000'000;
  std::uint64_t seed = 42;
  std::vector<std::size_t> threads;
  bool markdown = false;
};

Options parseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << arg << '\n';
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--ops") {
      options.ops = parseInteger<std::size_t>(next()).value_or(options.ops);
    } else if (arg == "--seed") {
      options.seed = parseInteger<std::uint64_t>(next()).value_or(options.seed);
    } else if (arg == "--threads") {
      const std::string list = next();  // Must outlive the string_views split() returns.
      for (const auto part : split(list, ',')) {
        if (const auto n = parseInteger<std::size_t>(part); n && *n > 0) {
          options.threads.push_back(*n);
        }
      }
    } else if (arg == "--quick") {
      options.ops = 100'000;
    } else if (arg == "--markdown") {
      options.markdown = true;
    } else {
      std::cerr << "usage: miniexchange_bench [--ops N] [--seed S] [--threads 1,2,4] [--quick] "
                   "[--markdown]\n";
      std::exit(2);
    }
  }
  if (options.threads.empty()) {
    const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    for (std::size_t t = 1; t <= std::min<std::size_t>(hw, 8); t *= 2) {
      options.threads.push_back(t);
    }
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parseOptions(argc, argv);
  std::cout << "MiniExchange benchmarks: ops=" << options.ops << " seed=" << options.seed
            << " hardware threads=" << std::thread::hardware_concurrency() << '\n'
            << "Workload: 70% limit (GTC/IOC), 8% market, 22% cancel, prices mid +/- 0.25\n";

  const std::vector<Operation> ops = generateWorkload(options.ops, options.seed);
  std::vector<Result> results;

  benchEngine<false>(ops);  // Warm-up: page in code and data.
  results.push_back(benchEngine<false>(ops));
  results.push_back(benchEngine<true>(ops));
  results.push_back(benchExchangeSingle(ops));

  for (const std::size_t threads : options.threads) {
    results.push_back(benchExchangeThreads(threads, options.ops / threads, options.seed, false));
  }
  for (const std::size_t threads : options.threads) {
    if (threads > 1) {
      results.push_back(benchExchangeThreads(threads, options.ops / threads, options.seed, true));
    }
  }
  results.push_back(benchGateway(std::min<std::size_t>(4, options.threads.back()),
                                 options.ops / 2, options.seed));

  results.push_back(benchAllocator<true>(options.ops * 5));
  results.push_back(benchAllocator<false>(options.ops * 5));

  printTable(results, options.markdown);
  std::cout << "\nLatency columns are per-operation wall time in nanoseconds (they include "
               "the clock read overhead).\n";
  return 0;
}
