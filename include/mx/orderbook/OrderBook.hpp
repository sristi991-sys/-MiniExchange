#pragma once

/// @file OrderBook.hpp
/// Limit order book for one symbol: separate bid and ask sides, each a sorted
/// map of price levels, plus a hash index from order id to resting order.
///
/// | Operation                 | Complexity                     |
/// |---------------------------|--------------------------------|
/// | add (existing level)      | O(log P) map lookup + O(1)     |
/// | cancel                    | O(1) average (+O(log P) if the level empties) |
/// | best bid / ask            | O(1)                           |
/// | fill front order          | O(1) (+O(log P) if the level empties) |
///
/// where P is the number of distinct price levels on one side.
///
/// The book is a pure data structure; the matching algorithm lives in
/// MatchingEngine. The book is not thread-safe: callers synchronise access.

#include "mx/core/Order.hpp"
#include "mx/orderbook/PriceLevel.hpp"
#include "mx/util/ObjectPool.hpp"

#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mx {

struct LevelInfo {
  Price price{0};
  Quantity quantity{0};
  std::size_t orderCount{0};
};

[[nodiscard]] inline bool operator==(const LevelInfo& a, const LevelInfo& b) noexcept {
  return a.price == b.price && a.quantity == b.quantity && a.orderCount == b.orderCount;
}

struct BookSnapshot {
  std::string symbol;
  std::vector<LevelInfo> bids;  ///< Best (highest) price first.
  std::vector<LevelInfo> asks;  ///< Best (lowest) price first.
};

class OrderBook {
 public:
  static constexpr std::size_t kAllLevels = std::numeric_limits<std::size_t>::max();

  explicit OrderBook(std::string symbol, std::size_t poolChunkSize = 4096);

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  ~OrderBook() = default;

  [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }

  // ---- Resting order management -------------------------------------------

  /// Adds an active limit order with open quantity to the back of its price level.
  /// @throws std::invalid_argument for market orders, inactive/empty orders,
  ///         non-positive prices, a different symbol or a duplicate id.
  const Order& add(const Order& order);

  /// Removes a resting order. Returns a copy of the cancelled order (status
  /// Cancelled, remaining quantity as it was), or std::nullopt if not found.
  std::optional<Order> cancel(OrderId id);

  [[nodiscard]] std::optional<Order> find(OrderId id) const;
  [[nodiscard]] bool contains(OrderId id) const noexcept;

  // ---- Market data ---------------------------------------------------------

  [[nodiscard]] std::optional<Price> bestBid() const noexcept;
  [[nodiscard]] std::optional<Price> bestAsk() const noexcept;
  [[nodiscard]] std::optional<Price> spread() const noexcept;
  [[nodiscard]] Quantity volumeAt(Side side, Price price) const noexcept;
  [[nodiscard]] Quantity totalVolume(Side side) const noexcept;
  [[nodiscard]] std::size_t levelCount(Side side) const noexcept;
  [[nodiscard]] std::size_t orderCount() const noexcept { return index_.size(); }
  [[nodiscard]] bool empty() const noexcept { return index_.empty(); }

  /// Aggregated depth, best prices first, at most `depth` levels per side.
  [[nodiscard]] BookSnapshot snapshot(std::size_t depth = kAllLevels) const;

  /// All resting orders on one side in priority order (price, then time).
  [[nodiscard]] std::vector<Order> orders(Side side) const;

  // ---- Matching primitives (used by MatchingEngine) ------------------------

  /// Highest-priority resting order on `side`, or nullptr if that side is empty.
  [[nodiscard]] OrderNode* bestOrder(Side side) noexcept;

  /// Fills `quantity` of a resting order. If it becomes fully filled it is
  /// removed from the book and its node returned to the pool, so `node` must
  /// not be used afterwards. Returns true if the order was removed.
  bool fill(OrderNode* node, Quantity quantity);

  /// Quantity resting on `restingSide` that an aggressor with the given limit
  /// (std::nullopt = market) could trade against, summed up to `cap`.
  [[nodiscard]] Quantity availableLiquidity(Side restingSide, std::optional<Price> limit,
                                            Quantity cap) const noexcept;

  /// Pre-sizes the pool and the id index for `orders` resting orders.
  void reserve(std::size_t orders);
  [[nodiscard]] std::size_t poolCapacity() const noexcept { return pool_.capacity(); }

 private:
  using NodePool = ObjectPool<OrderNode>;
  using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;  // best = highest
  using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;     // best = lowest

  PriceLevel& levelFor(Side side, Price price);
  void eraseLevel(Side side, Price price) noexcept;
  void unlink(OrderNode* node) noexcept;

  std::string symbol_;
  NodePool pool_;  // Declared before index_ so it is destroyed after every handle.
  BidLevels bids_;
  AskLevels asks_;
  std::unordered_map<OrderId, NodePool::Ptr> index_;  // Owns the resting orders.
};

}  // namespace mx
