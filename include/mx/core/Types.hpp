#pragma once

/// @file Types.hpp
/// Fundamental vocabulary types shared by every module of the exchange.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mx {

using OrderId = std::uint64_t;
using TradeId = std::uint64_t;

/// Prices are fixed-point integers measured in ticks (1 tick = 1 / kPriceScale of a
/// currency unit). Integer prices make comparisons exact and avoid the rounding
/// errors that floating point would introduce into price-time priority.
using Price = std::int64_t;
using Quantity = std::uint64_t;

/// Wall-clock nanoseconds since the Unix epoch.
using Timestamp = std::int64_t;

inline constexpr OrderId kInvalidOrderId = 0;
inline constexpr int kPriceDecimals = 2;
inline constexpr Price kPriceScale = 100;  // 10^kPriceDecimals
inline constexpr Price kMaxPrice = 1'000'000'000LL * kPriceScale;
inline constexpr Quantity kMaxOrderQuantity = 1'000'000'000ULL;
inline constexpr std::size_t kMaxSymbolLength = 16;
inline constexpr std::size_t kMaxTraderLength = 32;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
  Market,  ///< Executes immediately against the best available prices.
  Limit    ///< Executes only at the limit price or better.
};

enum class TimeInForce : std::uint8_t {
  GTC,  ///< Good-till-cancel: unfilled quantity rests in the book.
  IOC,  ///< Immediate-or-cancel: fill what is possible now, cancel the rest.
  FOK   ///< Fill-or-kill: fill the entire quantity immediately or nothing.
};

enum class OrderStatus : std::uint8_t { New, PartiallyFilled, Filled, Cancelled, Rejected };

[[nodiscard]] constexpr Side opposite(Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

[[nodiscard]] std::string_view toString(Side side) noexcept;
[[nodiscard]] std::string_view toString(OrderType type) noexcept;
[[nodiscard]] std::string_view toString(TimeInForce tif) noexcept;
[[nodiscard]] std::string_view toString(OrderStatus status) noexcept;

/// Case-insensitive parsers. They return std::nullopt for unknown text.
[[nodiscard]] std::optional<Side> parseSide(std::string_view text) noexcept;
[[nodiscard]] std::optional<OrderType> parseOrderType(std::string_view text) noexcept;
[[nodiscard]] std::optional<TimeInForce> parseTimeInForce(std::string_view text) noexcept;
[[nodiscard]] std::optional<OrderStatus> parseOrderStatus(std::string_view text) noexcept;

/// Parses a decimal price such as "150", "150.5" or "150.25" into ticks without
/// using floating point. Rejects signs, exponents, more than kPriceDecimals
/// fractional digits and values that would overflow.
[[nodiscard]] std::optional<Price> parsePrice(std::string_view text) noexcept;

/// Formats ticks as a decimal string with exactly kPriceDecimals decimals.
[[nodiscard]] std::string formatPrice(Price price);

/// Symbols: 1-16 characters from [A-Z0-9._-].
[[nodiscard]] bool isValidSymbol(std::string_view symbol) noexcept;

/// Trader ids: 0-32 characters from [A-Za-z0-9._-]. Empty means anonymous.
[[nodiscard]] bool isValidTrader(std::string_view trader) noexcept;

}  // namespace mx
