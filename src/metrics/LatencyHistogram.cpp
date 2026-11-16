#include "mx/metrics/LatencyHistogram.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace mx {
namespace {

constexpr auto kRelaxed = std::memory_order_relaxed;

/// Index of the most significant set bit. Precondition: value != 0.
unsigned highestBit(std::uint64_t value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  return 63U - static_cast<unsigned>(__builtin_clzll(value));
#elif defined(_MSC_VER)
  unsigned long index = 0;
  _BitScanReverse64(&index, value);
  return static_cast<unsigned>(index);
#else
  unsigned bit = 0;
  while (value >>= 1U) {
    ++bit;
  }
  return bit;
#endif
}

void atomicMin(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
  std::uint64_t current = target.load(kRelaxed);
  while (value < current && !target.compare_exchange_weak(current, value, kRelaxed)) {
  }
}

void atomicMax(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept {
  std::uint64_t current = target.load(kRelaxed);
  while (value > current && !target.compare_exchange_weak(current, value, kRelaxed)) {
  }
}

}  // namespace

LatencyHistogram::LatencyHistogram() noexcept : min_(std::numeric_limits<std::uint64_t>::max()) {
  for (auto& bucket : buckets_) {
    bucket.store(0, kRelaxed);
  }
}

std::size_t LatencyHistogram::bucketIndex(std::uint64_t value) noexcept {
  if (value < kSubBuckets) {
    return static_cast<std::size_t>(value);
  }
  const unsigned msb = highestBit(value);  // >= kSubBucketBits
  const unsigned shift = msb - kSubBucketBits;
  const auto sub = static_cast<std::size_t>((value >> shift) - kSubBuckets);  // 0..15
  return kSubBuckets + static_cast<std::size_t>(shift) * kSubBuckets + sub;
}

std::uint64_t LatencyHistogram::bucketUpperBound(std::size_t index) noexcept {
  if (index < kSubBuckets) {
    return index;
  }
  const std::size_t offset = index - kSubBuckets;
  const auto shift = static_cast<unsigned>(offset / kSubBuckets);
  const auto sub = static_cast<std::uint64_t>(offset % kSubBuckets);
  const std::uint64_t lower = (kSubBuckets + sub) << shift;
  return lower + ((std::uint64_t{1} << shift) - 1);
}

void LatencyHistogram::record(std::uint64_t value) noexcept {
  buckets_[bucketIndex(value)].fetch_add(1, kRelaxed);
  count_.fetch_add(1, kRelaxed);
  sum_.fetch_add(value, kRelaxed);
  atomicMin(min_, value);
  atomicMax(max_, value);
}

std::uint64_t LatencyHistogram::count() const noexcept { return count_.load(kRelaxed); }

std::uint64_t LatencyHistogram::min() const noexcept {
  return count() == 0 ? 0 : min_.load(kRelaxed);
}

std::uint64_t LatencyHistogram::max() const noexcept { return max_.load(kRelaxed); }

double LatencyHistogram::mean() const noexcept {
  const std::uint64_t n = count();
  return n == 0 ? 0.0 : static_cast<double>(sum_.load(kRelaxed)) / static_cast<double>(n);
}

std::uint64_t LatencyHistogram::percentile(double p) const noexcept {
  std::uint64_t total = 0;
  for (const auto& bucket : buckets_) {
    total += bucket.load(kRelaxed);
  }
  if (total == 0) {
    return 0;
  }
  p = std::clamp(p, 0.0, 100.0);
  auto target = static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(total)));
  target = std::max<std::uint64_t>(target, 1);

  std::uint64_t cumulative = 0;
  for (std::size_t i = 0; i < kBucketCount; ++i) {
    cumulative += buckets_[i].load(kRelaxed);
    if (cumulative >= target) {
      return std::min(bucketUpperBound(i), max());
    }
  }
  return max();
}

LatencySummary LatencyHistogram::summary() const noexcept {
  LatencySummary s;
  s.count = count();
  s.min = min();
  s.max = max();
  s.mean = mean();
  s.p50 = percentile(50.0);
  s.p90 = percentile(90.0);
  s.p99 = percentile(99.0);
  s.p999 = percentile(99.9);
  return s;
}

void LatencyHistogram::reset() noexcept {
  for (auto& bucket : buckets_) {
    bucket.store(0, kRelaxed);
  }
  count_.store(0, kRelaxed);
  sum_.store(0, kRelaxed);
  min_.store(std::numeric_limits<std::uint64_t>::max(), kRelaxed);
  max_.store(0, kRelaxed);
}

void LatencyHistogram::merge(const LatencyHistogram& other) noexcept {
  for (std::size_t i = 0; i < kBucketCount; ++i) {
    buckets_[i].fetch_add(other.buckets_[i].load(kRelaxed), kRelaxed);
  }
  count_.fetch_add(other.count_.load(kRelaxed), kRelaxed);
  sum_.fetch_add(other.sum_.load(kRelaxed), kRelaxed);
  if (other.count() > 0) {
    atomicMin(min_, other.min_.load(kRelaxed));
    atomicMax(max_, other.max_.load(kRelaxed));
  }
}

}  // namespace mx
