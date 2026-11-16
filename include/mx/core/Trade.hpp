#pragma once

/// @file Trade.hpp
/// An executed trade (a fill between one buy order and one sell order).

#include "mx/core/Types.hpp"

#include <string>

namespace mx {

struct Trade {
  TradeId id{0};
  std::string symbol;
  Price price{0};  ///< Execution price in ticks (always the resting order's price).
  Quantity quantity{0};
  OrderId buyOrderId{kInvalidOrderId};
  OrderId sellOrderId{kInvalidOrderId};
  std::string buyTrader;
  std::string sellTrader;
  Side aggressorSide{Side::Buy};  ///< Side of the incoming order that triggered the trade.
  Timestamp timestamp{0};
};

[[nodiscard]] bool operator==(const Trade& lhs, const Trade& rhs);
[[nodiscard]] bool operator!=(const Trade& lhs, const Trade& rhs);

[[nodiscard]] std::string describe(const Trade& trade);

}  // namespace mx
