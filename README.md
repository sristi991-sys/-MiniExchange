# MiniExchange: a high-performance stock exchange and order matching engine

A mini stock exchange written in modern C++ (C++17/C++20). It accepts **BUY**
and **SELL** orders for multiple stocks, keeps a separate **buy and sell order
book** per symbol, and matches orders automatically using the **price-time
priority** algorithm. It also executes trades with **partial fills**, supports
**market, limit, IOC and fill-or-kill** orders, handles **cancellations**, and
keeps a durable **transaction record**.

```text
mx> sell AAPL 50 150.25 carol
Order #2 SELL 50 AAPL @150.25 GTC -> NEW | resting 50
mx> buy AAPL 130 150.50 alice
Order #6 BUY 130 AAPL @150.50 GTC -> FILLED | filled 130 @ avg 150.27
  Trade #1 AAPL 50 @ 150.25 (buy #6 alice / sell #2 carol, aggressor BUY)
  Trade #2 AAPL 70 @ 150.25 (buy #6 alice / sell #3 dave, aggressor BUY)
  Trade #3 AAPL 10 @ 150.50 (buy #6 alice / sell #1 bob, aggressor BUY)
```

| | |
|---|---|
| **Matching engine** | 2.8M orders/s on one core, ~300 ns median per operation |
| **Full exchange, 8 threads** | 6.5M orders/s across independent symbols |
| **Tests** | 111 Google Tests, including a differential test against a reference model |
| **Toolchain** | CMake, GoogleTest, Docker, GitHub Actions (GCC, Clang, ASan, TSan) |

## Features

* **Order management**: limit and market orders; GTC, IOC and FOK time-in-force;
  validation; exchange-assigned unique ids; full order lifecycle
  (`NEW`, `PARTIALLY_FILLED`, `FILLED`, `CANCELLED`, `REJECTED`).
* **Order book**: bids and asks in sorted price levels, each level a FIFO
  queue; O(1) best bid/ask, O(1) cancel, depth snapshots.
* **Matching engine**: price-time priority, execution at the resting order's
  price (price improvement for the aggressor), multi-level sweeps, partial
  fills on both sides.
* **Trade management**: trade ledger, per-symbol statistics (open, high, low,
  last, volume, VWAP) and trade listeners (observer pattern).
* **Exchange management**: multiple symbols, a thread-safe API with
  per-symbol locking, a lock-free symbol table, and an asynchronous multi-threaded
  `OrderGateway` that returns `std::future`s.
* **Persistence**: a write-ahead event journal with deterministic crash
  recovery (event sourcing), plus CSV export/import of trades.
* **Performance monitoring**: lock-free latency histograms (p50, p90, p99,
  p99.9), throughput counters, and an RAII `ScopedTimer`.
* **Memory management**: a custom `ObjectPool` with `unique_ptr` RAII
  handles, intrusive lists, and no raw `new`/`delete` anywhere.

## Quick start

### Windows (MinGW-w64 GCC)

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT   # GCC 13+ (includes Ninja)
pip install cmake ninja                           # Kitware CMake
./scripts/build.ps1                               # configure + build + run tests
./scripts/build.ps1 -Bench                        # ...and run the benchmarks
```

The script puts the WinLibs GCC ahead of any older MinGW on `PATH`. Binaries
end up in `out/release-cpp20/bin/`.

### Linux / macOS

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Without presets:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Docker

```bash
docker build -t miniexchange .                                  # builds and runs the tests
docker run -it --rm miniexchange                                # interactive exchange
docker run --rm miniexchange --script /opt/miniexchange/demo.txt
docker run --rm --entrypoint miniexchange_bench miniexchange --quick
```

### Build options

| CMake option | Default | Meaning |
|---|---|---|
| `MX_CXX_STANDARD` | `20` | `17` or `20` |
| `MX_BUILD_TESTS` / `MX_BUILD_BENCHMARKS` / `MX_BUILD_APP` | `ON` | Choose which targets to build |
| `MX_WARNINGS_AS_ERRORS` | `OFF` (`ON` in presets/CI) | Add `-Werror` to the strict warning set |
| `MX_ENABLE_ASAN` / `MX_ENABLE_TSAN` | `OFF` | Build with AddressSanitizer + UBSan, or ThreadSanitizer |

GoogleTest comes from the system if it's installed (`find_package`); otherwise
CMake downloads v1.17.0 automatically.

## Using the exchange

```bash
miniexchange                                # interactive session
miniexchange --script scripts/demo.txt      # guided demo of every feature
miniexchange --journal session.journal      # durable: state survives restarts
miniexchange --symbols INFY,TCS,RELIANCE    # choose the listed symbols
```

| Command | Description |
|---|---|
| `buy <SYM> <QTY> <PRICE\|MKT> [GTC\|IOC\|FOK] [TRADER]` | Place a buy order (`MKT` = market) |
| `sell <SYM> <QTY> <PRICE\|MKT> [GTC\|IOC\|FOK] [TRADER]` | Place a sell order |
| `cancel <ORDER_ID> [SYM]` | Cancel a resting order |
| `book <SYM> [DEPTH]` | Order book ladder with the spread |
| `orders <SYM>` | Resting orders in priority order |
| `quote <SYM>` | Best bid and best ask |
| `trades [SYM] [N]` | Recent trades |
| `stats` | Per-symbol trades, volume, last, high, low, VWAP |
| `metrics` | Throughput and latency percentiles |
| `symbols`, `addsymbol <SYM>` | List symbols, or list a new one |
| `export <FILE.csv>` | Write the transaction record to CSV |
| `help`, `quit` | Show help, or exit |

### Using it as a library

```cpp
#include "mx/exchange/Exchange.hpp"

mx::Exchange exchange;
exchange.addSymbol("AAPL");

exchange.submitOrder(mx::OrderRequest::limit("AAPL", mx::Side::Sell, 100, 15025));  // 150.25
mx::ExecutionReport r =
    exchange.submitOrder(mx::OrderRequest::market("AAPL", mx::Side::Buy, 60));

// r.status == OrderStatus::Filled, r.trades[0].price == 15025
exchange.trades().subscribe([](const mx::Trade& t) { /* market data feed */ });
```

Prices are integer **ticks** (1 tick = 0.01). Use `mx::parsePrice("150.25")`
and `mx::formatPrice(15025)` to convert to and from text.

## Project layout

```
include/mx/
  core/         Types, Order, Trade, ExecutionReport, IdGenerator   (order management)
  orderbook/    PriceLevel, OrderBook                               (order book)
  matching/     MatchingEngine                                      (price-time matching)
  trade/        TradeManager                                        (ledger, statistics)
  exchange/     Exchange, OrderGateway                              (exchange management)
  persistence/  EventJournal, TradeStore                            (journal, recovery, CSV)
  metrics/      LatencyHistogram, PerformanceMonitor                (performance monitoring)
  util/         ObjectPool, SpinMutex, ThreadSafeQueue, Clock, StringUtils
src/            implementation, one static library per module
app/            interactive CLI (CommandProcessor + main)
tests/          111 Google Tests (13 files)
benchmarks/     reproducible benchmark harness
docs/           ARCHITECTURE.md, BENCHMARKS.md
scripts/        build.ps1 (Windows), demo.txt
```

## Testing

```bash
ctest --test-dir build --output-on-failure
./build/bin/miniexchange_tests --gtest_filter='*Matching*'
```

| Area | What it checks |
|---|---|
| Matching | Price priority, time priority, maker-price execution, sweeps, partial fills both ways, market/IOC/FOK, cancels, rejects |
| Reference model | 5 × 4,000 random operations checked against a naive O(n) model: every trade and the final book must match exactly |
| Order book | Best prices, level aggregation, O(1) cancel, snapshots, exception safety, pool reuse |
| Concurrency | 8 threads × 4 symbols, conserving every unit of quantity; readers during writes; gateway per-symbol ordering; queue MPMC |
| Persistence | CSV and journal round trips, torn-write tolerance, bit-exact recovery, multi-session journals |
| Metrics, pool, parsing, CLI | Histogram error bounds, RAII timer, pool chunking and exception safety, price parsing edge cases, every CLI command |

CI (`.github/workflows/ci.yml`) builds with GCC 13 and 14 and Clang 18, as
both C++17 and C++20, with `-Werror`. It also runs the whole suite under
**AddressSanitizer + UBSan** and **ThreadSanitizer**, and builds the Docker
image.

## Performance

Full numbers, the workload and the optimisation history are in
[docs/BENCHMARKS.md](docs/BENCHMARKS.md). Highlights from a 16-thread laptop:

| Scenario | Throughput |
|---|---:|
| Matching engine, 1 thread | 2.79 M ops/s (p50 303 ns, p99 1.4 µs) |
| Exchange, 1 thread | 1.25 M ops/s |
| Exchange, 8 threads, independent symbols | 6.53 M ops/s |
| Object pool vs `new`/`delete` | 55 M/s vs 12 M/s |

Measuring led to three optimisations, which together took 8-thread throughput
from 1.08M to 6.53M orders/s: a lock-free symbol table, per-thread striped
metrics, and an adaptive spin lock.

## Design

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) covers the module graph,
data structures and complexity, the matching algorithm, memory ownership,
the concurrency model and lock ordering, the journal format and recovery, and
known limitations.
