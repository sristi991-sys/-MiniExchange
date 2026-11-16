#pragma once

/// @file SpinMutex.hpp
/// Adaptive test-and-test-and-set spin lock for very short critical sections.
///
/// A matching step holds the book lock for a few hundred nanoseconds. With a
/// regular std::mutex a contended waiter is put to sleep by the kernel and
/// woken microseconds later, which costs far more than the critical section
/// itself. SpinMutex instead spins (reading, not writing, the lock word so the
/// cache line is not hammered) with a CPU pause hint, and only after a bounded
/// number of spins starts yielding its time slice so it cannot burn a core
/// indefinitely. Satisfies the Lockable requirements (works with lock_guard).

#include <atomic>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace mx {

inline void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#endif
}

class SpinMutex {
 public:
  SpinMutex() noexcept = default;
  SpinMutex(const SpinMutex&) = delete;
  SpinMutex& operator=(const SpinMutex&) = delete;

  void lock() noexcept {
    std::uint32_t spins = 0;
    while (locked_.exchange(true, std::memory_order_acquire)) {
      while (locked_.load(std::memory_order_relaxed)) {
        if (spins < kSpinsBeforeYield) {
          cpuRelax();
          ++spins;
        } else {
          std::this_thread::yield();
        }
      }
    }
  }

  [[nodiscard]] bool try_lock() noexcept {
    return !locked_.load(std::memory_order_relaxed) &&
           !locked_.exchange(true, std::memory_order_acquire);
  }

  void unlock() noexcept { locked_.store(false, std::memory_order_release); }

 private:
  static constexpr std::uint32_t kSpinsBeforeYield = 4096;
  std::atomic<bool> locked_{false};
};

}  // namespace mx
