#include "mx/exchange/OrderGateway.hpp"

#include "mx/exchange/Exchange.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mx {

OrderGateway::OrderGateway(Exchange& exchange, std::size_t workerCount) : exchange_(exchange) {
  if (workerCount == 0) {
    workerCount = std::max<std::size_t>(1, std::thread::hardware_concurrency());
  }
  queues_.reserve(workerCount);
  for (std::size_t i = 0; i < workerCount; ++i) {
    queues_.push_back(std::make_unique<ThreadSafeQueue<Task>>());
  }
  workers_.reserve(workerCount);
  try {
    for (std::size_t i = 0; i < workerCount; ++i) {
      workers_.emplace_back([this, i] { workerLoop(i); });
    }
  } catch (...) {
    shutdown();  // Join any threads that did start before rethrowing.
    throw;
  }
}

OrderGateway::~OrderGateway() { shutdown(); }

std::size_t OrderGateway::shardFor(const std::string& symbol) const noexcept {
  return std::hash<std::string>{}(symbol) % queues_.size();
}

template <typename Fn>
auto OrderGateway::enqueue(const std::string& symbol, Fn&& fn) {
  using Result = std::invoke_result_t<Fn>;
  // std::function needs a copyable callable, so the move-only packaged_task is
  // held through a shared_ptr.
  auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(fn));
  std::future<Result> future = task->get_future();
  if (stopped_.load(std::memory_order_acquire) ||
      !queues_[shardFor(symbol)]->push([task] { (*task)(); })) {
    throw std::runtime_error("OrderGateway has been shut down");
  }
  return future;
}

std::future<ExecutionReport> OrderGateway::submit(OrderRequest request) {
  const std::string symbol = request.symbol;
  return enqueue(symbol, [this, req = std::move(request)] { return exchange_.submitOrder(req); });
}

std::future<CancelResult> OrderGateway::cancel(std::string symbol, OrderId orderId) {
  const std::string shardKey = symbol;
  return enqueue(shardKey, [this, sym = std::move(symbol), orderId] {
    return exchange_.cancelOrder(sym, orderId);
  });
}

void OrderGateway::workerLoop(std::size_t index) {
  ThreadSafeQueue<Task>& queue = *queues_[index];
  while (auto task = queue.waitPop()) {
    (*task)();  // packaged_task captures exceptions into the future.
    tasksProcessed_.fetch_add(1, std::memory_order_relaxed);
  }
}

void OrderGateway::shutdown() {
  if (stopped_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  for (auto& queue : queues_) {
    queue->close();
  }
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

}  // namespace mx
