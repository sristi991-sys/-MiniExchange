# Benchmarks

## How to reproduce

```bash
./build/bin/miniexchange_bench            # 1,000,000 operations
./build/bin/miniexchange_bench --quick    # 100,000 operations
./build/bin/miniexchange_bench --ops 5000000 --threads 1,2,4,8,16 --markdown
```

**Workload** (seeded, generated before timing starts): 70% limit orders (90%
GTC, 10% IOC) priced at mid ± 0.25, so about half of them cross; 8% market
orders; 22% cancels of one of the client's last 2,000 orders. About 650K trades
per 1M operations.

**Machine**: Windows 11 laptop, 16 hardware threads, GCC 16.2 (WinLibs
MinGW-w64 UCRT), `-O3` Release build, C++20. Results vary by about ±10% between
runs, and the latency columns include the cost of reading the clock twice per
operation.

## Results

| Benchmark | Ops | Throughput | p50 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| MatchingEngine, 1 thread (raw throughput) | 1,000,000 | **2.79 M ops/s** | | | |
| MatchingEngine, 1 thread (per-op timing) | 1,000,000 | 2.04 M ops/s | 303 ns | 1.4 µs | 2.8 µs |
| Exchange, 1 thread (validation + locks + metrics) | 1,000,000 | 1.25 M ops/s | 607 ns | 2.0 µs | 4.4 µs |
| Exchange, 2 threads, 1 symbol each | 1,000,000 | 2.44 M ops/s | | | |
| Exchange, 4 threads, 1 symbol each | 1,000,000 | 4.06 M ops/s | | | |
| Exchange, 8 threads, 1 symbol each | 1,000,000 | **6.53 M ops/s** | | | |
| Exchange, 2 threads, 1 shared symbol | 1,000,000 | 1.52 M ops/s | | | |
| Exchange, 8 threads, 1 shared symbol | 1,000,000 | 0.93 M ops/s | | | |
| OrderGateway async (futures), 4 workers | 390,021 | 0.79 M ops/s | | | |
| `ObjectPool<OrderNode>` alloc + free | 5,000,000 | **55.2 M ops/s** | | | |
| `new`/`delete` OrderNode alloc + free | 5,000,000 | 11.6 M ops/s | | | |

## Optimisation history

The first multi-threaded run showed that the exchange **did not scale**: 8
threads on 8 *independent* symbols were no faster than 1 thread. Profiling the
hot path pointed to shared state that every order touched even when the
symbols differed. Each fix was measured with the same workload:

| Scenario | Baseline | + lock-free symbol table, striped metrics | + adaptive spin lock per book |
|---|---:|---:|---:|
| Exchange, 1 thread | 1.15 M/s | 1.28 M/s | 1.25 M/s |
| 2 threads, own symbols | 1.18 M/s | 1.82 M/s | 2.44 M/s |
| 4 threads, own symbols | 1.53 M/s | 2.89 M/s | 4.06 M/s |
| 8 threads, own symbols | 1.08 M/s | 5.41 M/s | **6.53 M/s (6x)** |
| 2 threads, shared symbol | 0.35 M/s | 0.37 M/s | **1.52 M/s (4.3x)** |
| 8 threads, shared symbol | 0.31 M/s | 0.35 M/s | 0.93 M/s (3x) |

1. **Symbol registry: from `std::shared_mutex` to a lock-free table.** Taking
   a *shared* lock still writes the lock's reader count, so every order from
   every thread bounced the same cache line. MinGW's rwlock is also built on a
   mutex internally. The registry became a fixed-capacity open-addressing table
   of `std::atomic<SymbolBook*>`: readers do one acquire load and write
   nothing.
2. **Metrics: from shared atomics to per-thread stripes.** Each order updated
   about 12 atomics on a few shared cache lines. Counters and histograms are now
   striped across 16 cache-line-aligned slots, one per thread, and summed when
   read.
3. **Book lock: from `std::mutex` to an adaptive spin lock.** A matching step
   holds the lock for about 300 ns, but a contended `std::mutex` puts the waiter to
   sleep in the kernel and wakes it microseconds later. `SpinMutex`
   (test-and-test-and-set with `pause`, falling back to `yield`) hands the lock
   over in nanoseconds.

## Interpretation

* The **raw engine** sustains about 2.8M operations per second on one core with
  a median of about 300 ns per operation, including matching, trade creation and
  pool recycling.
* The **full exchange path** roughly halves that. The extra cost comes from
  validation, two clock reads for metrics, the symbol lookup, locking and ledger
  updates. That is the price of a thread-safe, observable system.
* **Independent symbols scale nearly linearly** to 8 threads, because no
  shared cache line is written per order apart from the order-id counter.
* **A single hot symbol cannot scale with locking.** Time priority requires
  serialisation, so more threads only add handoff overhead. The `OrderGateway`
  answers this the way real exchanges do: one writer thread per instrument.
  Its throughput here is bounded by per-request future and `std::function`
  allocations and condition-variable wake-ups, which batching would reduce.
* **Pooling** makes order-node allocation about 4.8x faster than the general
  heap, and it keeps memory use flat during steady-state trading.
