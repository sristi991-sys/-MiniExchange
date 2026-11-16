#pragma once

#include "mx/core/Types.hpp"

#include <chrono>
#include <cstdint>

namespace mx {

/// Wall-clock nanoseconds since the Unix epoch (order and trade timestamps).
[[nodiscard]] inline Timestamp nowNanos() noexcept {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

/// Monotonic nanoseconds (latency measurement; never goes backwards).
[[nodiscard]] inline std::uint64_t steadyNanos() noexcept {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace mx
