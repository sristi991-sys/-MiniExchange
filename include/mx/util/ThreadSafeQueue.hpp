#pragma once

/// @file ThreadSafeQueue.hpp
/// Unbounded multi-producer / multi-consumer blocking queue.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace mx {

template <typename T>
class ThreadSafeQueue {
 public:
  ThreadSafeQueue() = default;
  ThreadSafeQueue(const ThreadSafeQueue&) = delete;
  ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

  /// Enqueues a value. Returns false (and drops the value) once the queue is closed.
  bool push(T value) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return false;
      }
      queue_.push_back(std::move(value));
    }
    ready_.notify_one();
    return true;
  }

  /// Blocks until a value is available or the queue is closed and drained.
  /// Returns std::nullopt only when closed and empty, so queued work is never lost.
  [[nodiscard]] std::optional<T> waitPop() {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = std::move(queue_.front());
    queue_.pop_front();
    return value;
  }

  [[nodiscard]] std::optional<T> tryPop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = std::move(queue_.front());
    queue_.pop_front();
    return value;
  }

  /// Wakes all waiting consumers; subsequent pushes fail.
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    ready_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  [[nodiscard]] bool empty() const { return size() == 0; }

 private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<T> queue_;
  bool closed_{false};
};

}  // namespace mx
