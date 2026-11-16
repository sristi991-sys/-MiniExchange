#include "mx/metrics/PerformanceMonitor.hpp"

#include <iomanip>
#include <sstream>

namespace mx {
namespace {

constexpr auto kRelaxed = std::memory_order_relaxed;

std::string micros(std::uint64_t nanos) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << static_cast<double>(nanos) / 1000.0 << " us";
  return out.str();
}

void writeLatency(std::ostringstream& out, const char* label, const LatencySummary& s) {
  out << "  " << label << " (n=" << s.count << "): ";
  if (s.count == 0) {
    out << "no samples\n";
    return;
  }
  out << "min " << micros(s.min) << ", p50 " << micros(s.p50) << ", p90 " << micros(s.p90)
      << ", p99 " << micros(s.p99) << ", p99.9 " << micros(s.p999) << ", max " << micros(s.max)
      << ", mean " << micros(static_cast<std::uint64_t>(s.mean)) << '\n';
}

}  // namespace

PerformanceMonitor::PerformanceMonitor()
    : stripes_(std::make_unique<Stripe[]>(kStripes)), startNanos_(steadyNanos()) {}

PerformanceMonitor::Stripe& PerformanceMonitor::localStripe() noexcept {
  // Threads are assigned stripes round-robin the first time they record.
  static std::atomic<std::size_t> nextStripe{0};
  thread_local const std::size_t index = nextStripe.fetch_add(1, kRelaxed) % kStripes;
  return stripes_[index];
}

void PerformanceMonitor::recordOrder(std::uint64_t latencyNanos, bool accepted,
                                     std::size_t tradeCount, Quantity volume) noexcept {
  Stripe& stripe = localStripe();
  stripe.orderLatency.record(latencyNanos);
  stripe.ordersReceived.fetch_add(1, kRelaxed);
  (accepted ? stripe.ordersAccepted : stripe.ordersRejected).fetch_add(1, kRelaxed);
  if (tradeCount > 0) {
    stripe.trades.fetch_add(tradeCount, kRelaxed);
    stripe.volume.fetch_add(volume, kRelaxed);
  }
}

void PerformanceMonitor::recordCancel(std::uint64_t latencyNanos, bool success) noexcept {
  Stripe& stripe = localStripe();
  stripe.cancelLatency.record(latencyNanos);
  stripe.cancelRequests.fetch_add(1, kRelaxed);
  if (success) {
    stripe.cancelsSucceeded.fetch_add(1, kRelaxed);
  }
}

MetricsSnapshot PerformanceMonitor::snapshot() const noexcept {
  MetricsSnapshot s;
  LatencyHistogram orders;
  LatencyHistogram cancels;
  for (std::size_t i = 0; i < kStripes; ++i) {
    const Stripe& stripe = stripes_[i];
    s.ordersReceived += stripe.ordersReceived.load(kRelaxed);
    s.ordersAccepted += stripe.ordersAccepted.load(kRelaxed);
    s.ordersRejected += stripe.ordersRejected.load(kRelaxed);
    s.cancelRequests += stripe.cancelRequests.load(kRelaxed);
    s.cancelsSucceeded += stripe.cancelsSucceeded.load(kRelaxed);
    s.trades += stripe.trades.load(kRelaxed);
    s.volume += stripe.volume.load(kRelaxed);
    orders.merge(stripe.orderLatency);
    cancels.merge(stripe.cancelLatency);
  }
  s.uptimeSeconds = static_cast<double>(steadyNanos() - startNanos_.load(kRelaxed)) / 1e9;
  s.ordersPerSecond =
      s.uptimeSeconds > 0 ? static_cast<double>(s.ordersReceived) / s.uptimeSeconds : 0.0;
  s.orderLatency = orders.summary();
  s.cancelLatency = cancels.summary();
  return s;
}

std::string PerformanceMonitor::report() const {
  const MetricsSnapshot s = snapshot();
  std::ostringstream out;
  out << "Performance metrics (uptime " << std::fixed << std::setprecision(1) << s.uptimeSeconds
      << " s)\n";
  out << "  orders received: " << s.ordersReceived << " (accepted " << s.ordersAccepted
      << ", rejected " << s.ordersRejected << ")\n";
  out << "  cancels: " << s.cancelRequests << " requested, " << s.cancelsSucceeded
      << " succeeded\n";
  out << "  trades: " << s.trades << ", volume: " << s.volume << '\n';
  out << "  average rate: " << std::setprecision(0) << s.ordersPerSecond << " orders/s\n";
  writeLatency(out, "order latency", s.orderLatency);
  writeLatency(out, "cancel latency", s.cancelLatency);
  return out.str();
}

void PerformanceMonitor::reset() noexcept {
  for (std::size_t i = 0; i < kStripes; ++i) {
    Stripe& stripe = stripes_[i];
    stripe.orderLatency.reset();
    stripe.cancelLatency.reset();
    stripe.ordersReceived.store(0, kRelaxed);
    stripe.ordersAccepted.store(0, kRelaxed);
    stripe.ordersRejected.store(0, kRelaxed);
    stripe.cancelRequests.store(0, kRelaxed);
    stripe.cancelsSucceeded.store(0, kRelaxed);
    stripe.trades.store(0, kRelaxed);
    stripe.volume.store(0, kRelaxed);
  }
  startNanos_.store(steadyNanos(), kRelaxed);
}

}  // namespace mx
