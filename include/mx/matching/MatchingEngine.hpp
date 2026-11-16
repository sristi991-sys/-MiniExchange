#pragma once

/// @file MatchingEngine.hpp
/// Price-time priority matching for a single symbol.
///
/// Algorithm (for an incoming BUY; SELL is symmetric):
///   1. FOK only: if the asks priced <= limit cannot fill the whole quantity,
///      cancel without trading.
///   2. While quantity remains and the best ask crosses (ask <= limit, or any
///      ask for a market order): trade min(remaining, resting) with the oldest
///      order at the best ask, at the *resting* order's price.
///   3. Leftover quantity: GTC limit orders rest in the book; IOC, FOK and
///      market orders have the remainder cancelled.
///
/// Price priority comes from the sorted price-level maps, time priority from
/// the FIFO queue inside each level. The engine is not thread-safe; the
/// Exchange serialises access per symbol.

#include "mx/core/ExecutionReport.hpp"
#include "mx/core/IdGenerator.hpp"
#include "mx/core/Order.hpp"
#include "mx/orderbook/OrderBook.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mx {

class MatchingEngine {
 public:
  /// Standalone engine with its own trade id sequence starting at 1.
  explicit MatchingEngine(std::string symbol);
  /// Engine sharing an exchange-wide trade id sequence.
  MatchingEngine(std::string symbol, IdGenerator& tradeIds);

  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;

  /// Matches an order (id already assigned) against the book and returns
  /// what happened. Never throws for bad orders: they are reported as Rejected.
  [[nodiscard]] ExecutionReport process(Order order);

  /// Cancels a resting order.
  CancelResult cancel(OrderId id);

  [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
  [[nodiscard]] const std::string& symbol() const noexcept { return book_.symbol(); }
  [[nodiscard]] std::uint64_t ordersProcessed() const noexcept { return ordersProcessed_; }
  [[nodiscard]] std::uint64_t tradesExecuted() const noexcept { return tradesExecuted_; }

  /// Pre-sizes internal structures for `orders` resting orders.
  void reserve(std::size_t orders) { book_.reserve(orders); }

 private:
  [[nodiscard]] static bool crosses(const Order& incoming, Price restingPrice) noexcept;
  [[nodiscard]] static ExecutionReport reject(Order& order, std::string reason);
  void match(Order& incoming, std::vector<Trade>& trades);
  [[nodiscard]] Trade makeTrade(const Order& incoming, const Order& resting, Quantity quantity,
                                Timestamp timestamp);

  std::unique_ptr<IdGenerator> ownedTradeIds_;
  IdGenerator* tradeIds_;
  OrderBook book_;
  std::uint64_t ordersProcessed_{0};
  std::uint64_t tradesExecuted_{0};
};

}  // namespace mx
