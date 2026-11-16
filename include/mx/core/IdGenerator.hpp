#pragma once

/// @file IdGenerator.hpp
/// Lock-free, thread-safe monotonically increasing id source.

#include <atomic>
#include <cstdint>

namespace mx {

class IdGenerator {
 public:
  explicit IdGenerator(std::uint64_t first = 1) noexcept : next_(first) {}

  IdGenerator(const IdGenerator&) = delete;
  IdGenerator& operator=(const IdGenerator&) = delete;

  /// Returns a new unique id. Uniqueness only needs atomicity, not ordering
  /// with other memory, so relaxed ordering is sufficient.
  [[nodiscard]] std::uint64_t next() noexcept {
    return next_.fetch_add(1, std::memory_order_relaxed);
  }

  /// The id the next call to next() will return (racy under concurrency).
  [[nodiscard]] std::uint64_t peek() const noexcept {
    return next_.load(std::memory_order_relaxed);
  }

  /// Guarantees that future ids are strictly greater than `used`. Used after
  /// replaying a journal so that new orders never reuse a recovered id.
  void ensureAbove(std::uint64_t used) noexcept {
    std::uint64_t current = next_.load(std::memory_order_relaxed);
    while (current <= used &&
           !next_.compare_exchange_weak(current, used + 1, std::memory_order_relaxed)) {
    }
  }

 private:
  std::atomic<std::uint64_t> next_;
};

}  // namespace mx
