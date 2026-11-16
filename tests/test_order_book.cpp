#include "mx/orderbook/OrderBook.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

using namespace mx;

namespace {

Order limit(OrderId id, Side side, Price price, Quantity quantity, std::string symbol = "TEST") {
  return Order(id, std::move(symbol), side, OrderType::Limit, TimeInForce::GTC, price, quantity);
}

std::vector<OrderId> idsOf(const std::vector<Order>& orders) {
  std::vector<OrderId> out;
  for (const Order& o : orders) {
    out.push_back(o.id());
  }
  return out;
}

}  // namespace

TEST(OrderBook, EmptyBook) {
  const OrderBook book("TEST");
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.symbol(), "TEST");
  EXPECT_FALSE(book.bestBid().has_value());
  EXPECT_FALSE(book.bestAsk().has_value());
  EXPECT_FALSE(book.spread().has_value());
  EXPECT_EQ(book.levelCount(Side::Buy), 0u);
  EXPECT_EQ(book.totalVolume(Side::Sell), 0u);
}

TEST(OrderBook, TracksBestPricesOnBothSides) {
  OrderBook book("TEST");
  book.add(limit(1, Side::Buy, 100, 10));
  book.add(limit(2, Side::Buy, 101, 10));
  book.add(limit(3, Side::Buy, 99, 10));
  book.add(limit(4, Side::Sell, 105, 10));
  book.add(limit(5, Side::Sell, 103, 10));
  EXPECT_EQ(book.bestBid(), Price{101});
  EXPECT_EQ(book.bestAsk(), Price{103});
  EXPECT_EQ(book.spread(), Price{2});
  EXPECT_EQ(book.levelCount(Side::Buy), 3u);
  EXPECT_EQ(book.levelCount(Side::Sell), 2u);
  EXPECT_EQ(book.orderCount(), 5u);
}

TEST(OrderBook, AggregatesQuantityPerLevel) {
  OrderBook book("TEST");
  book.add(limit(1, Side::Sell, 100, 10));
  book.add(limit(2, Side::Sell, 100, 15));
  book.add(limit(3, Side::Sell, 101, 7));
  EXPECT_EQ(book.volumeAt(Side::Sell, 100), 25u);
  EXPECT_EQ(book.volumeAt(Side::Sell, 101), 7u);
  EXPECT_EQ(book.volumeAt(Side::Sell, 102), 0u);
  EXPECT_EQ(book.volumeAt(Side::Buy, 100), 0u);
  EXPECT_EQ(book.totalVolume(Side::Sell), 32u);
}

TEST(OrderBook, CancelRemovesOrderAndEmptyLevel) {
  OrderBook book("TEST");
  book.add(limit(1, Side::Buy, 100, 10));
  book.add(limit(2, Side::Buy, 100, 5));
  book.add(limit(3, Side::Buy, 99, 5));

  const auto cancelled = book.cancel(1);
  ASSERT_TRUE(cancelled.has_value());
  EXPECT_EQ(cancelled->status(), OrderStatus::Cancelled);
  EXPECT_EQ(cancelled->remainingQuantity(), 10u);
  EXPECT_EQ(book.volumeAt(Side::Buy, 100), 5u);
  EXPECT_FALSE(book.contains(1));

  book.cancel(2);
  EXPECT_EQ(book.levelCount(Side::Buy), 1u);
  EXPECT_EQ(book.bestBid(), Price{99});
}

TEST(OrderBook, CancelUnknownOrderReturnsNullopt) {
  OrderBook book("TEST");
  EXPECT_FALSE(book.cancel(42).has_value());
  book.add(limit(1, Side::Buy, 100, 10));
  book.cancel(1);
  EXPECT_FALSE(book.cancel(1).has_value());
}

TEST(OrderBook, FindReturnsCopyOfRestingOrder) {
  OrderBook book("TEST");
  book.add(limit(7, Side::Sell, 250, 3));
  const auto found = book.find(7);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->price(), 250);
  EXPECT_EQ(found->remainingQuantity(), 3u);
  EXPECT_FALSE(book.find(8).has_value());
}

TEST(OrderBook, RejectsInvalidOrdersWithoutChangingState) {
  OrderBook book("TEST");
  book.add(limit(1, Side::Buy, 100, 10));
  EXPECT_THROW(book.add(limit(1, Side::Buy, 101, 10)), std::invalid_argument);  // duplicate
  EXPECT_THROW(book.add(limit(2, Side::Buy, 100, 10, "OTHER")), std::invalid_argument);
  EXPECT_THROW(book.add(limit(3, Side::Buy, 0, 10)), std::invalid_argument);
  EXPECT_THROW(book.add(limit(4, Side::Buy, 100, 0)), std::invalid_argument);
  EXPECT_THROW(book.add(Order(5, "TEST", Side::Buy, OrderType::Market, TimeInForce::IOC, 0, 10)),
               std::invalid_argument);
  Order cancelled = limit(6, Side::Buy, 100, 10);
  cancelled.cancel();
  EXPECT_THROW(book.add(cancelled), std::invalid_argument);

  EXPECT_EQ(book.orderCount(), 1u);
  EXPECT_EQ(book.levelCount(Side::Buy), 1u);
  EXPECT_EQ(book.volumeAt(Side::Buy, 100), 10u);
}

TEST(OrderBook, SnapshotListsBestLevelsFirst) {
  OrderBook book("TEST");
  book.add(limit(1, Side::Buy, 98, 1));
  book.add(limit(2, Side::Buy, 100, 2));
  book.add(limit(3, Side::Buy, 99, 3));
  book.add(limit(4, Side::Sell, 103, 4));
  book.add(limit(5, Side::Sell, 101, 5));
  book.add(limit(6, Side::Sell, 101, 6));

  const BookSnapshot full = book.snapshot();
  ASSERT_EQ(full.bids.size(), 3u);
  EXPECT_EQ(full.bids[0].price, 100);
  EXPECT_EQ(full.bids[1].price, 99);
  EXPECT_EQ(full.bids[2].price, 98);
  ASSERT_EQ(full.asks.size(), 2u);
  EXPECT_EQ(full.asks[0], (LevelInfo{101, 11, 2}));
  EXPECT_EQ(full.asks[1], (LevelInfo{103, 4, 1}));

  const BookSnapshot top = book.snapshot(1);
  EXPECT_EQ(top.bids.size(), 1u);
  EXPECT_EQ(top.asks.size(), 1u);
}

TEST(OrderBook, OrdersAreListedInPriceTimePriority) {
  OrderBook book("TEST");
  book.add(limit(1, Side::Sell, 102, 1));
  book.add(limit(2, Side::Sell, 101, 1));
  book.add(limit(3, Side::Sell, 101, 1));
  book.add(limit(4, Side::Sell, 100, 1));
  EXPECT_EQ(idsOf(book.orders(Side::Sell)), (std::vector<OrderId>{4, 2, 3, 1}));

  book.add(limit(5, Side::Buy, 90, 1));
  book.add(limit(6, Side::Buy, 95, 1));
  book.add(limit(7, Side::Buy, 95, 1));
  EXPECT_EQ(idsOf(book.orders(Side::Buy)), (std::vector<OrderId>{6, 7, 5}));
}

TEST(OrderBook, FillReducesThenRemovesRestingOrder) {
  OrderBook book("TEST");
  book.add(limit(1, Side::Sell, 100, 10));
  OrderNode* node = book.bestOrder(Side::Sell);
  ASSERT_NE(node, nullptr);
  EXPECT_FALSE(book.fill(node, 4));
  EXPECT_EQ(book.volumeAt(Side::Sell, 100), 6u);
  EXPECT_EQ(book.find(1)->status(), OrderStatus::PartiallyFilled);
  EXPECT_THROW(book.fill(node, 7), std::invalid_argument);  // Overfill is rejected.
  EXPECT_TRUE(book.fill(node, 6));
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.bestOrder(Side::Sell), nullptr);
}

TEST(OrderBook, AvailableLiquidityRespectsLimitAndCap) {
  OrderBook book("TEST");
  book.add(limit(1, Side::Sell, 100, 10));
  book.add(limit(2, Side::Sell, 101, 10));
  book.add(limit(3, Side::Sell, 102, 10));
  EXPECT_EQ(book.availableLiquidity(Side::Sell, Price{101}, 1000), 20u);
  EXPECT_EQ(book.availableLiquidity(Side::Sell, Price{99}, 1000), 0u);
  EXPECT_EQ(book.availableLiquidity(Side::Sell, std::nullopt, 1000), 30u);
  EXPECT_LE(book.availableLiquidity(Side::Sell, std::nullopt, 5), 10u);  // Stops early at cap.

  book.add(limit(4, Side::Buy, 95, 5));
  book.add(limit(5, Side::Buy, 94, 5));
  EXPECT_EQ(book.availableLiquidity(Side::Buy, Price{95}, 1000), 5u);
  EXPECT_EQ(book.availableLiquidity(Side::Buy, Price{90}, 1000), 10u);
}

TEST(OrderBook, ReusesPoolMemoryAcrossChurn) {
  OrderBook book("TEST", 64);
  for (int round = 0; round < 100; ++round) {
    for (OrderId id = 1; id <= 64; ++id) {
      book.add(limit(id, Side::Buy, 100 + static_cast<Price>(id % 5), 1));
    }
    for (OrderId id = 1; id <= 64; ++id) {
      book.cancel(id);
    }
  }
  EXPECT_TRUE(book.empty());
  EXPECT_EQ(book.poolCapacity(), 64u);  // One chunk, recycled 100 times.
}
