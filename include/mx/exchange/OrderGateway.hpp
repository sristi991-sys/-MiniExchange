#pragma once

/// @file OrderGateway.hpp
/// Asynchronous, multi-threaded front door to the Exchange.
///
/// Requests are sharded onto worker threads by symbol hash. Each worker owns a
/// FIFO queue, so all requests for one symbol are processed by one thread in
/// submission order (per-symbol sequencing, as on a real exchange), while
/// different symbols are processed in parallel. Results are delivered through
/// std::future. Worker threads are owned with RAII: the destructor drains the
/// queues and joins the threads.

#include "mx/core/ExecutionReport.hpp"
#include "mx/core/Order.hpp"
#include "mx/util/ThreadSafeQueue.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mx {

class Exchange;

class OrderGateway {
 public:
  /// @param workerCount number of worker threads; 0 = hardware concurrency.
  explicit OrderGateway(Exchange& exchange, std::size_t workerCount = 0);
  ~OrderGateway();

  OrderGateway(const OrderGateway&) = delete;
  OrderGateway& operator=(const OrderGateway&) = delete;

  /// @throws std::runtime_error after shutdown().
  [[nodiscard]] std::future<ExecutionReport> submit(OrderRequest request);
  /// @throws std::runtime_error after shutdown().
  [[nodiscard]] std::future<CancelResult> cancel(std::string symbol, OrderId orderId);

  /// Stops accepting work, finishes everything already queued and joins the
  /// workers. Idempotent.
  void shutdown();

  [[nodiscard]] std::size_t workerCount() const noexcept { return workers_.size(); }
  [[nodiscard]] std::uint64_t tasksProcessed() const noexcept {
    return tasksProcessed_.load(std::memory_order_relaxed);
  }

 private:
  using Task = std::function<void()>;

  [[nodiscard]] std::size_t shardFor(const std::string& symbol) const noexcept;
  template <typename Fn>
  auto enqueue(const std::string& symbol, Fn&& fn);
  void workerLoop(std::size_t index);

  Exchange& exchange_;
  std::vector<std::unique_ptr<ThreadSafeQueue<Task>>> queues_;
  std::vector<std::thread> workers_;
  std::atomic<std::uint64_t> tasksProcessed_{0};
  std::atomic<bool> stopped_{false};
};

}  // namespace mx
