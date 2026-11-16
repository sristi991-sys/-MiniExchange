#include "mx/persistence/TradeStore.hpp"

#include "mx/util/StringUtils.hpp"

#include <fstream>
#include <stdexcept>

namespace mx {

std::string_view tradeCsvHeader() noexcept {
  return "trade_id,timestamp,symbol,price,quantity,buy_order_id,sell_order_id,buy_trader,"
         "sell_trader,aggressor";
}

std::string toCsvRow(const Trade& trade) {
  std::string row;
  row += std::to_string(trade.id);
  row += ',';
  row += std::to_string(trade.timestamp);
  row += ',';
  row += trade.symbol;
  row += ',';
  row += formatPrice(trade.price);
  row += ',';
  row += std::to_string(trade.quantity);
  row += ',';
  row += std::to_string(trade.buyOrderId);
  row += ',';
  row += std::to_string(trade.sellOrderId);
  row += ',';
  row += trade.buyTrader;
  row += ',';
  row += trade.sellTrader;
  row += ',';
  row += toString(trade.aggressorSide);
  return row;
}

std::optional<Trade> parseTradeCsvRow(std::string_view row) {
  const auto fields = split(trim(row), ',');
  if (fields.size() != 10) {
    return std::nullopt;
  }
  const auto id = parseInteger<TradeId>(fields[0]);
  const auto timestamp = parseInteger<Timestamp>(fields[1]);
  const auto price = parsePrice(fields[3]);
  const auto quantity = parseInteger<Quantity>(fields[4]);
  const auto buyId = parseInteger<OrderId>(fields[5]);
  const auto sellId = parseInteger<OrderId>(fields[6]);
  const auto aggressor = parseSide(fields[9]);
  if (!id || !timestamp || !isValidSymbol(fields[2]) || !price || !quantity || !buyId ||
      !sellId || !isValidTrader(fields[7]) || !isValidTrader(fields[8]) || !aggressor) {
    return std::nullopt;
  }
  Trade trade;
  trade.id = *id;
  trade.timestamp = *timestamp;
  trade.symbol = std::string(fields[2]);
  trade.price = *price;
  trade.quantity = *quantity;
  trade.buyOrderId = *buyId;
  trade.sellOrderId = *sellId;
  trade.buyTrader = std::string(fields[7]);
  trade.sellTrader = std::string(fields[8]);
  trade.aggressorSide = *aggressor;
  return trade;
}

void writeTradesCsv(const std::string& path, const std::vector<Trade>& trades) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    throw std::runtime_error("cannot open '" + path + "' for writing");
  }
  out << tradeCsvHeader() << '\n';
  for (const Trade& trade : trades) {
    out << toCsvRow(trade) << '\n';
  }
  out.flush();
  if (!out) {
    throw std::runtime_error("failed while writing '" + path + "'");
  }
}

std::vector<Trade> readTradesCsv(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error("cannot open '" + path + "' for reading");
  }
  std::vector<Trade> trades;
  std::string line;
  std::size_t lineNumber = 0;
  while (std::getline(in, line)) {
    ++lineNumber;
    const std::string_view view = trim(line);
    if (view.empty() || (lineNumber == 1 && view == tradeCsvHeader())) {
      continue;
    }
    auto trade = parseTradeCsvRow(view);
    if (!trade) {
      throw std::runtime_error("malformed trade row at " + path + ":" +
                               std::to_string(lineNumber));
    }
    trades.push_back(std::move(*trade));
  }
  return trades;
}

}  // namespace mx
