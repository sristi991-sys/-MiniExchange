#include "mx/core/Types.hpp"

#include <cctype>
#include <cstdint>
#include <limits>

namespace mx {
namespace {

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::toupper(static_cast<unsigned char>(a[i])) !=
        std::toupper(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string_view toString(Side side) noexcept { return side == Side::Buy ? "BUY" : "SELL"; }

std::string_view toString(OrderType type) noexcept {
  return type == OrderType::Market ? "MARKET" : "LIMIT";
}

std::string_view toString(TimeInForce tif) noexcept {
  switch (tif) {
    case TimeInForce::GTC:
      return "GTC";
    case TimeInForce::IOC:
      return "IOC";
    case TimeInForce::FOK:
      return "FOK";
  }
  return "UNKNOWN";
}

std::string_view toString(OrderStatus status) noexcept {
  switch (status) {
    case OrderStatus::New:
      return "NEW";
    case OrderStatus::PartiallyFilled:
      return "PARTIALLY_FILLED";
    case OrderStatus::Filled:
      return "FILLED";
    case OrderStatus::Cancelled:
      return "CANCELLED";
    case OrderStatus::Rejected:
      return "REJECTED";
  }
  return "UNKNOWN";
}

std::optional<Side> parseSide(std::string_view text) noexcept {
  if (iequals(text, "BUY") || iequals(text, "B")) {
    return Side::Buy;
  }
  if (iequals(text, "SELL") || iequals(text, "S")) {
    return Side::Sell;
  }
  return std::nullopt;
}

std::optional<OrderType> parseOrderType(std::string_view text) noexcept {
  if (iequals(text, "MARKET") || iequals(text, "MKT")) {
    return OrderType::Market;
  }
  if (iequals(text, "LIMIT") || iequals(text, "LMT")) {
    return OrderType::Limit;
  }
  return std::nullopt;
}

std::optional<TimeInForce> parseTimeInForce(std::string_view text) noexcept {
  if (iequals(text, "GTC")) {
    return TimeInForce::GTC;
  }
  if (iequals(text, "IOC")) {
    return TimeInForce::IOC;
  }
  if (iequals(text, "FOK")) {
    return TimeInForce::FOK;
  }
  return std::nullopt;
}

std::optional<OrderStatus> parseOrderStatus(std::string_view text) noexcept {
  for (const auto status : {OrderStatus::New, OrderStatus::PartiallyFilled, OrderStatus::Filled,
                            OrderStatus::Cancelled, OrderStatus::Rejected}) {
    if (iequals(text, toString(status))) {
      return status;
    }
  }
  return std::nullopt;
}

std::optional<Price> parsePrice(std::string_view text) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  constexpr Price kMaxWhole = std::numeric_limits<Price>::max() / kPriceScale - 1;
  Price whole = 0;
  Price fraction = 0;
  int fractionDigits = 0;
  bool seenDot = false;
  bool seenDigit = false;

  for (const char c : text) {
    if (c == '.') {
      if (seenDot) {
        return std::nullopt;
      }
      seenDot = true;
      continue;
    }
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    seenDigit = true;
    const Price digit = c - '0';
    if (!seenDot) {
      if (whole > (kMaxWhole - digit) / 10) {
        return std::nullopt;
      }
      whole = whole * 10 + digit;
    } else {
      if (++fractionDigits > kPriceDecimals) {
        return std::nullopt;
      }
      fraction = fraction * 10 + digit;
    }
  }
  if (!seenDigit) {
    return std::nullopt;
  }
  for (int i = fractionDigits; i < kPriceDecimals; ++i) {
    fraction *= 10;
  }
  return whole * kPriceScale + fraction;
}

std::string formatPrice(Price price) {
  const bool negative = price < 0;
  // Negate in unsigned arithmetic so that the minimum value cannot overflow.
  const std::uint64_t magnitude =
      negative ? 0 - static_cast<std::uint64_t>(price) : static_cast<std::uint64_t>(price);
  const auto scale = static_cast<std::uint64_t>(kPriceScale);

  std::string out = negative ? "-" : "";
  out += std::to_string(magnitude / scale);
  out += '.';
  const std::string fraction = std::to_string(magnitude % scale);
  out.append(static_cast<std::size_t>(kPriceDecimals) - fraction.size(), '0');
  out += fraction;
  return out;
}

bool isValidSymbol(std::string_view symbol) noexcept {
  if (symbol.empty() || symbol.size() > kMaxSymbolLength) {
    return false;
  }
  for (const char c : symbol) {
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                    c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool isValidTrader(std::string_view trader) noexcept {
  if (trader.size() > kMaxTraderLength) {
    return false;
  }
  for (const char c : trader) {
    const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' ||
                    c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

}  // namespace mx
