#include "mx/orderbook/PriceLevel.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace mx;

namespace {

Order order(OrderId id, Quantity quantity) {
  return Order(id, "TEST", Side::Buy, OrderType::Limit, TimeInForce::GTC, 100, quantity);
}

std::vector<OrderId> ids(const PriceLevel& level) {
  std::vector<OrderId> out;
  level.forEach([&out](const Order& o) { out.push_back(o.id()); });
  return out;
}

}  // namespace

TEST(PriceLevel, StartsEmpty) {
  const PriceLevel level(100);
  EXPECT_TRUE(level.empty());
  EXPECT_EQ(level.price(), 100);
  EXPECT_EQ(level.totalQuantity(), 0u);
  EXPECT_EQ(level.orderCount(), 0u);
  EXPECT_EQ(level.front(), nullptr);
}

TEST(PriceLevel, MaintainsFifoOrderAndTotals) {
  PriceLevel level(100);
  OrderNode a(order(1, 10));
  OrderNode b(order(2, 20));
  OrderNode c(order(3, 30));
  level.pushBack(&a);
  level.pushBack(&b);
  level.pushBack(&c);
  EXPECT_EQ(level.front(), &a);
  EXPECT_EQ(level.back(), &c);
  EXPECT_EQ(level.totalQuantity(), 60u);
  EXPECT_EQ(level.orderCount(), 3u);
  EXPECT_EQ(ids(level), (std::vector<OrderId>{1, 2, 3}));
  EXPECT_EQ(a.level, &level);
}

TEST(PriceLevel, RemovesFromHeadMiddleAndTail) {
  PriceLevel level(100);
  OrderNode a(order(1, 10));
  OrderNode b(order(2, 20));
  OrderNode c(order(3, 30));
  OrderNode d(order(4, 40));
  for (OrderNode* node : {&a, &b, &c, &d}) {
    level.pushBack(node);
  }

  level.remove(&b);  // middle
  EXPECT_EQ(ids(level), (std::vector<OrderId>{1, 3, 4}));
  EXPECT_EQ(b.prev, nullptr);
  EXPECT_EQ(b.next, nullptr);
  EXPECT_EQ(b.level, nullptr);

  level.remove(&a);  // head
  EXPECT_EQ(level.front(), &c);
  level.remove(&d);  // tail
  EXPECT_EQ(level.back(), &c);
  EXPECT_EQ(level.totalQuantity(), 30u);
  level.remove(&c);
  EXPECT_TRUE(level.empty());
  EXPECT_EQ(level.totalQuantity(), 0u);
  EXPECT_EQ(level.back(), nullptr);
}

TEST(PriceLevel, ReduceQuantityTracksPartialFills) {
  PriceLevel level(100);
  OrderNode a(order(1, 10));
  level.pushBack(&a);
  a.order.fill(4);
  level.reduceQuantity(4);
  EXPECT_EQ(level.totalQuantity(), 6u);
  level.remove(&a);  // Subtracts the remaining 6.
  EXPECT_EQ(level.totalQuantity(), 0u);
}
