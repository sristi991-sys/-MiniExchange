#include "mx/orderbook/OrderBook.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mx {
namespace {

template <typename Levels>
std::vector<LevelInfo> collectLevels(const Levels& levels, std::size_t depth) {
  std::vector<LevelInfo> out;
  out.reserve(std::min(depth, levels.size()));
  for (const auto& entry : levels) {
    if (out.size() >= depth) {
      break;
    }
    const PriceLevel& level = entry.second;
    out.push_back(LevelInfo{level.price(), level.totalQuantity(), level.orderCount()});
  }
  return out;
}

template <typename Levels>
void collectOrders(const Levels& levels, std::vector<Order>& out) {
  for (const auto& entry : levels) {
    entry.second.forEach([&out](const Order& order) { out.push_back(order); });
  }
}

template <typename Levels>
Quantity sumVolume(const Levels& levels) noexcept {
  Quantity total = 0;
  for (const auto& entry : levels) {
    total += entry.second.totalQuantity();
  }
  return total;
}

/// Walks levels best-first while they are acceptable to the aggressor.
template <typename Levels>
Quantity sumLiquidity(const Levels& levels, Side restingSide, std::optional<Price> limit,
                      Quantity cap) noexcept {
  Quantity total = 0;
  for (const auto& entry : levels) {
    const Price price = entry.first;
    if (limit) {
      // Resting asks must be <= the buyer's limit; resting bids >= the seller's limit.
      const bool acceptable = restingSide == Side::Sell ? price <= *limit : price >= *limit;
      if (!acceptable) {
        break;
      }
    }
    total += entry.second.totalQuantity();
    if (total >= cap) {
      break;
    }
  }
  return total;
}

template <typename Levels>
Quantity volumeAtPrice(const Levels& levels, Price price) noexcept {
  const auto it = levels.find(price);
  return it == levels.end() ? 0 : it->second.totalQuantity();
}

}  // namespace

OrderBook::OrderBook(std::string symbol, std::size_t poolChunkSize)
    : symbol_(std::move(symbol)), pool_(poolChunkSize) {}

const Order& OrderBook::add(const Order& order) {
  if (!order.isLimit()) {
    throw std::invalid_argument("only limit orders can rest in the book");
  }
  if (!order.isActive() || order.remainingQuantity() == 0) {
    throw std::invalid_argument("order " + std::to_string(order.id()) + " has no open quantity");
  }
  if (order.price() <= 0) {
    throw std::invalid_argument("resting order must have a positive price");
  }
  if (order.symbol() != symbol_) {
    throw std::invalid_argument("order symbol '" + order.symbol() + "' does not match book '" +
                                symbol_ + "'");
  }

  auto [it, inserted] = index_.try_emplace(order.id());
  if (!inserted) {
    throw std::invalid_argument("duplicate order id " + std::to_string(order.id()));
  }
  try {
    it->second = pool_.make(order);
    levelFor(order.side(), order.price()).pushBack(it->second.get());
  } catch (...) {
    index_.erase(it);  // Strong guarantee: the book is unchanged on failure.
    throw;
  }
  return it->second->order;
}

std::optional<Order> OrderBook::cancel(OrderId id) {
  const auto it = index_.find(id);
  if (it == index_.end()) {
    return std::nullopt;
  }
  OrderNode* node = it->second.get();
  node->order.cancel();
  std::optional<Order> cancelled(node->order);
  unlink(node);
  index_.erase(it);  // Returns the node to the pool.
  return cancelled;
}

std::optional<Order> OrderBook::find(OrderId id) const {
  const auto it = index_.find(id);
  if (it == index_.end()) {
    return std::nullopt;
  }
  return it->second->order;
}

bool OrderBook::contains(OrderId id) const noexcept { return index_.find(id) != index_.end(); }

std::optional<Price> OrderBook::bestBid() const noexcept {
  if (bids_.empty()) {
    return std::nullopt;
  }
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const noexcept {
  if (asks_.empty()) {
    return std::nullopt;
  }
  return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const noexcept {
  const auto bid = bestBid();
  const auto ask = bestAsk();
  if (!bid || !ask) {
    return std::nullopt;
  }
  return *ask - *bid;
}

Quantity OrderBook::volumeAt(Side side, Price price) const noexcept {
  return side == Side::Buy ? volumeAtPrice(bids_, price) : volumeAtPrice(asks_, price);
}

Quantity OrderBook::totalVolume(Side side) const noexcept {
  return side == Side::Buy ? sumVolume(bids_) : sumVolume(asks_);
}

std::size_t OrderBook::levelCount(Side side) const noexcept {
  return side == Side::Buy ? bids_.size() : asks_.size();
}

BookSnapshot OrderBook::snapshot(std::size_t depth) const {
  BookSnapshot snap;
  snap.symbol = symbol_;
  snap.bids = collectLevels(bids_, depth);
  snap.asks = collectLevels(asks_, depth);
  return snap;
}

std::vector<Order> OrderBook::orders(Side side) const {
  std::vector<Order> out;
  if (side == Side::Buy) {
    collectOrders(bids_, out);
  } else {
    collectOrders(asks_, out);
  }
  return out;
}

OrderNode* OrderBook::bestOrder(Side side) noexcept {
  if (side == Side::Buy) {
    return bids_.empty() ? nullptr : bids_.begin()->second.front();
  }
  return asks_.empty() ? nullptr : asks_.begin()->second.front();
}

bool OrderBook::fill(OrderNode* node, Quantity quantity) {
  node->order.fill(quantity);  // Validates; throws before any book state changes.
  node->level->reduceQuantity(quantity);
  if (!node->order.isFilled()) {
    return false;
  }
  const OrderId id = node->order.id();
  unlink(node);
  index_.erase(id);  // Node goes back to the pool here.
  return true;
}

Quantity OrderBook::availableLiquidity(Side restingSide, std::optional<Price> limit,
                                       Quantity cap) const noexcept {
  return restingSide == Side::Buy ? sumLiquidity(bids_, restingSide, limit, cap)
                                  : sumLiquidity(asks_, restingSide, limit, cap);
}

void OrderBook::reserve(std::size_t orders) {
  pool_.reserve(orders);
  index_.reserve(orders);
}

PriceLevel& OrderBook::levelFor(Side side, Price price) {
  if (side == Side::Buy) {
    return bids_.try_emplace(price, price).first->second;
  }
  return asks_.try_emplace(price, price).first->second;
}

void OrderBook::eraseLevel(Side side, Price price) noexcept {
  if (side == Side::Buy) {
    bids_.erase(price);
  } else {
    asks_.erase(price);
  }
}

void OrderBook::unlink(OrderNode* node) noexcept {
  PriceLevel* level = node->level;
  const Side side = node->order.side();
  const Price price = level->price();
  level->remove(node);
  if (level->empty()) {
    eraseLevel(side, price);
  }
}

}  // namespace mx
