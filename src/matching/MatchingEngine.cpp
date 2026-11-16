#include "mx/matching/MatchingEngine.hpp"

#include "mx/util/Clock.hpp"

#include <algorithm>
#include <utility>

namespace mx {

MatchingEngine::MatchingEngine(std::string symbol)
    : ownedTradeIds_(std::make_unique<IdGenerator>(1)),
      tradeIds_(ownedTradeIds_.get()),
      book_(std::move(symbol)) {}

MatchingEngine::MatchingEngine(std::string symbol, IdGenerator& tradeIds)
    : tradeIds_(&tradeIds), book_(std::move(symbol)) {}

ExecutionReport MatchingEngine::process(Order order) {
  ++ordersProcessed_;

  // Defensive checks: the Exchange validates requests, but the engine is also a
  // public component and must never corrupt the book.
  if (order.symbol() != book_.symbol()) {
    return reject(order, "order symbol '" + order.symbol() + "' does not match engine '" +
                             book_.symbol() + "'");
  }
  if (!order.isActive() || order.remainingQuantity() == 0) {
    return reject(order, "order has no open quantity");
  }
  if (order.isLimit() && order.price() <= 0) {
    return reject(order, "limit price must be positive");
  }
  if (book_.contains(order.id())) {
    return reject(order, "duplicate order id " + std::to_string(order.id()));
  }

  // Fill-or-kill: check liquidity first so a kill leaves the book untouched.
  if (order.timeInForce() == TimeInForce::FOK) {
    const std::optional<Price> limit =
        order.isLimit() ? std::optional<Price>(order.price()) : std::nullopt;
    const Quantity available =
        book_.availableLiquidity(opposite(order.side()), limit, order.quantity());
    if (available < order.quantity()) {
      order.cancel();
      ExecutionReport report = ExecutionReport::fromOrder(order);
      report.cancelledQuantity = order.remainingQuantity();
      report.message = "FOK order killed: only " + std::to_string(available) + " of " +
                       std::to_string(order.quantity()) + " available";
      return report;
    }
  }

  std::vector<Trade> trades;
  match(order, trades);

  Quantity resting = 0;
  Quantity cancelled = 0;
  std::string message;
  if (order.remainingQuantity() > 0) {
    if (order.isLimit() && order.timeInForce() == TimeInForce::GTC) {
      book_.add(order);
      resting = order.remainingQuantity();
    } else {
      cancelled = order.remainingQuantity();
      order.cancel();
      message = order.isMarket() ? "unfilled market quantity cancelled: insufficient liquidity"
                                 : "unfilled IOC quantity cancelled";
    }
  }

  ExecutionReport report = ExecutionReport::fromOrder(order);
  report.restingQuantity = resting;
  report.cancelledQuantity = cancelled;
  report.trades = std::move(trades);
  report.message = std::move(message);
  return report;
}

CancelResult MatchingEngine::cancel(OrderId id) {
  CancelResult result;
  result.orderId = id;
  result.symbol = book_.symbol();
  if (const auto cancelled = book_.cancel(id)) {
    result.success = true;
    result.cancelledQuantity = cancelled->remainingQuantity();
    result.message = "cancelled";
  } else {
    result.message = "order not found (unknown, already filled or already cancelled)";
  }
  return result;
}

bool MatchingEngine::crosses(const Order& incoming, Price restingPrice) noexcept {
  if (incoming.isMarket()) {
    return true;
  }
  return incoming.isBuy() ? restingPrice <= incoming.price() : restingPrice >= incoming.price();
}

ExecutionReport MatchingEngine::reject(Order& order, std::string reason) {
  order.reject();
  ExecutionReport report = ExecutionReport::fromOrder(order);
  report.status = OrderStatus::Rejected;
  report.message = std::move(reason);
  return report;
}

void MatchingEngine::match(Order& incoming, std::vector<Trade>& trades) {
  const Side restingSide = opposite(incoming.side());
  // Trades are stamped with the aggressor's arrival time, so replaying the
  // journal reproduces them bit for bit. Orders created without a timestamp
  // (direct engine use) fall back to one wall-clock read per incoming order.
  Timestamp tradeTime = incoming.timestamp();

  while (incoming.remainingQuantity() > 0) {
    OrderNode* node = book_.bestOrder(restingSide);
    if (node == nullptr || !crosses(incoming, node->order.price())) {
      break;
    }
    const Quantity quantity = std::min(incoming.remainingQuantity(), node->order.remainingQuantity());
    if (tradeTime == 0) {
      tradeTime = nowNanos();
    }
    // Build the trade before filling: a fully filled resting node is released.
    trades.push_back(makeTrade(incoming, node->order, quantity, tradeTime));
    incoming.fill(quantity);
    book_.fill(node, quantity);
    ++tradesExecuted_;
  }
}

Trade MatchingEngine::makeTrade(const Order& incoming, const Order& resting, Quantity quantity,
                                Timestamp timestamp) {
  const Order& buy = incoming.isBuy() ? incoming : resting;
  const Order& sell = incoming.isBuy() ? resting : incoming;

  Trade trade;
  trade.id = tradeIds_->next();
  trade.symbol = book_.symbol();
  trade.price = resting.price();  // The maker sets the price; the taker may get improvement.
  trade.quantity = quantity;
  trade.buyOrderId = buy.id();
  trade.sellOrderId = sell.id();
  trade.buyTrader = buy.trader();
  trade.sellTrader = sell.trader();
  trade.aggressorSide = incoming.side();
  trade.timestamp = timestamp;
  return trade;
}

}  // namespace mx
