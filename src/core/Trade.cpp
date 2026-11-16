#include "mx/core/Trade.hpp"

#include <sstream>

namespace mx {

bool operator==(const Trade& lhs, const Trade& rhs) {
  return lhs.id == rhs.id && lhs.symbol == rhs.symbol && lhs.price == rhs.price &&
         lhs.quantity == rhs.quantity && lhs.buyOrderId == rhs.buyOrderId &&
         lhs.sellOrderId == rhs.sellOrderId && lhs.buyTrader == rhs.buyTrader &&
         lhs.sellTrader == rhs.sellTrader && lhs.aggressorSide == rhs.aggressorSide &&
         lhs.timestamp == rhs.timestamp;
}

bool operator!=(const Trade& lhs, const Trade& rhs) { return !(lhs == rhs); }

std::string describe(const Trade& trade) {
  std::ostringstream out;
  out << "Trade #" << trade.id << ' ' << trade.symbol << ' ' << trade.quantity << " @ "
      << formatPrice(trade.price) << " (buy #" << trade.buyOrderId;
  if (!trade.buyTrader.empty()) {
    out << ' ' << trade.buyTrader;
  }
  out << " / sell #" << trade.sellOrderId;
  if (!trade.sellTrader.empty()) {
    out << ' ' << trade.sellTrader;
  }
  out << ", aggressor " << toString(trade.aggressorSide) << ')';
  return out.str();
}

}  // namespace mx
