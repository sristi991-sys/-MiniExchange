#pragma once

/// @file TradeStore.hpp
/// CSV export/import of the transaction record (executed trades).
///
/// Columns: trade_id,timestamp,symbol,price,quantity,buy_order_id,
///          sell_order_id,buy_trader,sell_trader,aggressor
/// Prices are written as decimals (e.g. 150.25) so the file opens cleanly in a
/// spreadsheet, and are parsed back exactly without floating point.

#include "mx/core/Trade.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mx {

[[nodiscard]] std::string_view tradeCsvHeader() noexcept;
[[nodiscard]] std::string toCsvRow(const Trade& trade);
[[nodiscard]] std::optional<Trade> parseTradeCsvRow(std::string_view row);

/// @throws std::runtime_error if the file cannot be written.
void writeTradesCsv(const std::string& path, const std::vector<Trade>& trades);

/// @throws std::runtime_error if the file cannot be read or a row is malformed.
[[nodiscard]] std::vector<Trade> readTradesCsv(const std::string& path);

}  // namespace mx
