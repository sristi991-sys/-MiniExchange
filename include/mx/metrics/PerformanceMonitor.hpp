#pragma once

/// @file PerformanceMonitor.hpp
/// Performance monitoring module: throughput counters and latency histograms.
///
/// Recording is lock-free *and* contention-free: the counters are striped
/// across kStripes cache-line-aligned slots and each thread writes to its own
/// stripe, so concurrent threads never bounce a shared cache line. Readers sum
/// the stripes. (A single set of shared atomics was measured to stop the
/// exchange from scaling with thread count; see docs/BENCHMARKS.md.)

#include "mx/core/Types.hpp"
#include "mx/metrics/LatencyHistogram.hpp"
#include "mx/util/Clock.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace mx {

struct MetricsSnapshot {
  std::uint64_t ordersReceived{0};
  std::uint64_t ordersAccepted{0};
  std::uint64_t ordersRejected{0};
  std::uint64_t cancelRequests{0};
  std::uint64_t cancelsSucceeded{0};
  std::uint64_t trades{0};
  std::uint64_t volume{0};
  double uptimeSeconds{0.0};
  double ordersPerSecond{0.0};
  LatencySummary orderLatency;   ///< Nanoseconds per submitOrder call.
  LatencySummary cancelLatency;  ///< Nanoseconds per cancelOrder call.
};

class PerformanceMonitor {
 public:
  static constexpr std::size_t kStripes = 16;

  PerformanceMonitor();
  PerformanceMonitor(const PerformanceMonitor&) = delete;
  PerformanceMonitor& operator=(const PerformanceMonitor&) = delete;

  void recordOrder(std::uint64_t latencyNanos, bool accepted, std::size_t tradeCount,
                   Quantity volume) noexcept;
  void recordCancel(std::uint64_t latencyNanos, bool success) noexcept;

  [[nodiscard]] MetricsSnapshot snapshot() const noexcept;
  /// Multi-line, human-readable summary.
  [[nodiscard]] std::string report() const;
  void reset() noexcept;

 private:
  /// One thread's slice of the counters, padded to whole cache lines.
  struct alignas(64) Stripe {
    LatencyHistogram orderLatency;
    LatencyHistogram cancelLatency;
    std::atomic<std::uint64_t> ordersReceived{0};
    std::atomic<std::uint64_t> ordersAccepted{0};
    std::atomic<std::uint64_t> ordersRejected{0};
    std::atomic<std::uint64_t> cancelRequests{0};
    std::atomic<std::uint64_t> cancelsSucceeded{0};
    std::atomic<std::uint64_t> trades{0};
    std::atomic<std::uint64_t> volume{0};
  };

  [[nodiscard]] Stripe& localStripe() noexcept;

  std::unique_ptr<Stripe[]> stripes_;
  std::atomic<std::uint64_t> startNanos_;
};

/// RAII timer: records the elapsed time into a histogram when it goes out of scope.
class ScopedTimer {
 public:
  explicit ScopedTimer(LatencyHistogram& histogram) noexcept
      : histogram_(histogram), start_(steadyNanos()) {}
  ~ScopedTimer() { histogram_.record(elapsedNanos()); }

  ScopedTimer(const ScopedTimer&) = delete;
  ScopedTimer& operator=(const ScopedTimer&) = delete;

  [[nodiscard]] std::uint64_t elapsedNanos() const noexcept { return steadyNanos() - start_; }

 private:
  LatencyHistogram& histogram_;
  std::uint64_t start_;
};

}  // namespace mx
