#include "mx/metrics/LatencyHistogram.hpp"
#include "mx/metrics/PerformanceMonitor.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

using namespace mx;

TEST(LatencyHistogram, EmptyHistogramReportsZeros) {
  const LatencyHistogram h;
  EXPECT_EQ(h.count(), 0u);
  EXPECT_EQ(h.min(), 0u);
  EXPECT_EQ(h.max(), 0u);
  EXPECT_EQ(h.percentile(99), 0u);
  EXPECT_DOUBLE_EQ(h.mean(), 0.0);
}

TEST(LatencyHistogram, SmallValuesAreExact) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 10; ++v) {
    h.record(v);
  }
  EXPECT_EQ(h.count(), 10u);
  EXPECT_EQ(h.min(), 1u);
  EXPECT_EQ(h.max(), 10u);
  EXPECT_EQ(h.percentile(50), 5u);
  EXPECT_EQ(h.percentile(100), 10u);
  EXPECT_DOUBLE_EQ(h.mean(), 5.5);
}

TEST(LatencyHistogram, BucketsCoverTheFullRangeContiguously) {
  EXPECT_EQ(LatencyHistogram::bucketIndex(0), 0u);
  EXPECT_EQ(LatencyHistogram::bucketIndex(UINT64_MAX), LatencyHistogram::kBucketCount - 1);
  EXPECT_EQ(LatencyHistogram::bucketUpperBound(LatencyHistogram::kBucketCount - 1), UINT64_MAX);
  for (std::size_t i = 0; i + 1 < LatencyHistogram::kBucketCount; ++i) {
    const std::uint64_t upper = LatencyHistogram::bucketUpperBound(i);
    ASSERT_EQ(LatencyHistogram::bucketIndex(upper), i);
    ASSERT_EQ(LatencyHistogram::bucketIndex(upper + 1), i + 1);
  }
}

TEST(LatencyHistogram, PercentilesWithinRelativeErrorBound) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 100'000; ++v) {
    h.record(v);
  }
  for (const double p : {50.0, 90.0, 99.0, 99.9}) {
    const double exact = p / 100.0 * 100'000;
    const double reported = static_cast<double>(h.percentile(p));
    EXPECT_GE(reported, exact * 0.999) << "p" << p;
    EXPECT_LE(reported, exact * 1.0625 + 1) << "p" << p;  // <= 6.25% bucket width.
  }
}

TEST(LatencyHistogram, ResetAndMerge) {
  LatencyHistogram a;
  LatencyHistogram b;
  a.record(100);
  b.record(5);
  b.record(1000);
  a.merge(b);
  EXPECT_EQ(a.count(), 3u);
  EXPECT_EQ(a.min(), 5u);
  EXPECT_EQ(a.max(), 1000u);
  a.reset();
  EXPECT_EQ(a.count(), 0u);
  EXPECT_EQ(a.percentile(50), 0u);
}

TEST(LatencyHistogram, ConcurrentRecordingLosesNothing) {
  LatencyHistogram h;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&h] {
      for (std::uint64_t i = 0; i < 25'000; ++i) {
        h.record(i % 1000);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(h.count(), 100'000u);
  EXPECT_EQ(h.max(), 999u);
}

TEST(PerformanceMonitor, CountsOrdersCancelsAndTrades) {
  PerformanceMonitor monitor;
  monitor.recordOrder(1000, true, 2, 50);
  monitor.recordOrder(2000, false, 0, 0);
  monitor.recordCancel(500, true);
  monitor.recordCancel(700, false);

  const MetricsSnapshot s = monitor.snapshot();
  EXPECT_EQ(s.ordersReceived, 2u);
  EXPECT_EQ(s.ordersAccepted, 1u);
  EXPECT_EQ(s.ordersRejected, 1u);
  EXPECT_EQ(s.cancelRequests, 2u);
  EXPECT_EQ(s.cancelsSucceeded, 1u);
  EXPECT_EQ(s.trades, 2u);
  EXPECT_EQ(s.volume, 50u);
  EXPECT_EQ(s.orderLatency.count, 2u);
  EXPECT_EQ(s.orderLatency.max, 2000u);
  EXPECT_NE(monitor.report().find("orders received: 2"), std::string::npos);

  monitor.reset();
  EXPECT_EQ(monitor.snapshot().ordersReceived, 0u);
}

TEST(ScopedTimer, RecordsOnDestruction) {
  LatencyHistogram h;
  {
    ScopedTimer timer(h);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(h.count(), 1u);
  EXPECT_GE(h.max(), 1'000'000u);  // At least 1 ms in nanoseconds.
}
