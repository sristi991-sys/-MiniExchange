#pragma once

/// @file LatencyHistogram.hpp
/// Lock-free log-linear histogram for latency percentiles.
///
/// Values 0..15 get exact buckets; above that each power of two is split into
/// 16 linear sub-buckets, giving <= 6.25% relative error across the full
/// 64-bit range with fixed memory (976 counters) and an O(1), wait-free
/// record() that many threads can call concurrently.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace mx {

struct LatencySummary {
  std::uint64_t count{0};
  std::uint64_t min{0};
  std::uint64_t max{0};
  std::uint64_t p50{0};
  std::uint64_t p90{0};
  std::uint64_t p99{0};
  std::uint64_t p999{0};
  double mean{0.0};
};

class LatencyHistogram {
 public:
  static constexpr unsigned kSubBucketBits = 4;
  static constexpr std::size_t kSubBuckets = std::size_t{1} << kSubBucketBits;
  static constexpr std::size_t kBucketCount = kSubBuckets + (64 - kSubBucketBits) * kSubBuckets;

  LatencyHistogram() noexcept;
  LatencyHistogram(const LatencyHistogram&) = delete;
  LatencyHistogram& operator=(const LatencyHistogram&) = delete;

  void record(std::uint64_t value) noexcept;

  [[nodiscard]] std::uint64_t count() const noexcept;
  [[nodiscard]] std::uint64_t min() const noexcept;  ///< 0 when empty.
  [[nodiscard]] std::uint64_t max() const noexcept;
  [[nodiscard]] double mean() const noexcept;
  /// Value at percentile p (0-100), reported as the upper bound of its bucket.
  [[nodiscard]] std::uint64_t percentile(double p) const noexcept;
  [[nodiscard]] LatencySummary summary() const noexcept;

  void reset() noexcept;
  void merge(const LatencyHistogram& other) noexcept;

  [[nodiscard]] static std::size_t bucketIndex(std::uint64_t value) noexcept;
  [[nodiscard]] static std::uint64_t bucketUpperBound(std::size_t index) noexcept;

 private:
  std::array<std::atomic<std::uint64_t>, kBucketCount> buckets_;
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::uint64_t> sum_{0};
  std::atomic<std::uint64_t> min_;
  std::atomic<std::uint64_t> max_{0};
};

}  // namespace mx
