#include "mx/core/ExecutionReport.hpp"
#include "mx/core/Order.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace mx;

namespace {
Order makeLimit(Quantity quantity = 100) {
  return Order(1, "TEST", Side::Buy, OrderType::Limit, TimeInForce::GTC, 15025, quantity, "alice",
               123);
}
}  // namespace

TEST(Order, NewOrderStartsWithFullQuantity) {
  const Order order = makeLimit();
  EXPECT_EQ(order.id(), 1u);
  EXPECT_EQ(order.symbol(), "TEST");
  EXPECT_EQ(order.trader(), "alice");
  EXPECT_EQ(order.price(), 15025);
  EXPECT_EQ(order.quantity(), 100u);
  EXPECT_EQ(order.remainingQuantity(), 100u);
  EXPECT_EQ(order.filledQuantity(), 0u);
  EXPECT_EQ(order.status(), OrderStatus::New);
  EXPECT_EQ(order.timestamp(), 123);
  EXPECT_TRUE(order.isActive());
  EXPECT_TRUE(order.isBuy());
  EXPECT_TRUE(order.isLimit());
}

TEST(Order, PartialThenFullFill) {
  Order order = makeLimit();
  order.fill(40);
  EXPECT_EQ(order.status(), OrderStatus::PartiallyFilled);
  EXPECT_EQ(order.remainingQuantity(), 60u);
  EXPECT_EQ(order.filledQuantity(), 40u);
  order.fill(60);
  EXPECT_EQ(order.status(), OrderStatus::Filled);
  EXPECT_TRUE(order.isFilled());
  EXPECT_FALSE(order.isActive());
}

TEST(Order, InvalidFillQuantitiesThrowAndLeaveStateUnchanged) {
  Order order = makeLimit(10);
  EXPECT_THROW(order.fill(0), std::invalid_argument);
  EXPECT_THROW(order.fill(11), std::invalid_argument);
  EXPECT_EQ(order.remainingQuantity(), 10u);
  EXPECT_EQ(order.status(), OrderStatus::New);
}

TEST(Order, CannotFillInactiveOrders) {
  Order order = makeLimit(10);
  order.cancel();
  EXPECT_EQ(order.status(), OrderStatus::Cancelled);
  EXPECT_EQ(order.remainingQuantity(), 10u);  // Kept for reporting.
  EXPECT_THROW(order.fill(1), std::logic_error);

  Order filled = makeLimit(5);
  filled.fill(5);
  filled.cancel();  // No-op once filled.
  EXPECT_EQ(filled.status(), OrderStatus::Filled);
}

TEST(Order, RejectOnlyAppliesToNewOrders) {
  Order fresh = makeLimit();
  fresh.reject();
  EXPECT_EQ(fresh.status(), OrderStatus::Rejected);

  Order partial = makeLimit();
  partial.fill(1);
  partial.reject();
  EXPECT_EQ(partial.status(), OrderStatus::PartiallyFilled);
}

TEST(Order, MarketOrdersAreNormalised) {
  const Order order(7, "TEST", Side::Sell, OrderType::Market, TimeInForce::GTC, 999, 10);
  EXPECT_EQ(order.price(), 0);
  EXPECT_EQ(order.timeInForce(), TimeInForce::IOC);
  const Order fok(8, "TEST", Side::Sell, OrderType::Market, TimeInForce::FOK, 0, 10);
  EXPECT_EQ(fok.timeInForce(), TimeInForce::FOK);
}

TEST(Order, ConstructFromRequest) {
  const auto request = OrderRequest::limit("MSFT", Side::Sell, 25, 30000, TimeInForce::IOC, "bob");
  const Order order(42, request, 99);
  EXPECT_EQ(order.id(), 42u);
  EXPECT_EQ(order.symbol(), "MSFT");
  EXPECT_EQ(order.side(), Side::Sell);
  EXPECT_EQ(order.timeInForce(), TimeInForce::IOC);
  EXPECT_EQ(order.trader(), "bob");
  EXPECT_NE(order.describe().find("#42 SELL 25 MSFT @300.00 IOC"), std::string::npos);
}

TEST(OrderValidation, AcceptsValidRequests) {
  EXPECT_FALSE(validationError(OrderRequest::limit("AAPL", Side::Buy, 1, 1)).has_value());
  EXPECT_FALSE(validationError(OrderRequest::market("AAPL", Side::Sell, 5, "t1")).has_value());
}

TEST(OrderValidation, RejectsBadRequests) {
  EXPECT_TRUE(validationError(OrderRequest::limit("AAPL", Side::Buy, 0, 100)).has_value());
  EXPECT_TRUE(validationError(OrderRequest::limit("AAPL", Side::Buy, 10, 0)).has_value());
  EXPECT_TRUE(validationError(OrderRequest::limit("AAPL", Side::Buy, 10, -5)).has_value());
  EXPECT_TRUE(validationError(OrderRequest::limit("", Side::Buy, 10, 100)).has_value());
  EXPECT_TRUE(validationError(OrderRequest::limit("aapl", Side::Buy, 10, 100)).has_value());
  EXPECT_TRUE(
      validationError(OrderRequest::limit("AAPL", Side::Buy, kMaxOrderQuantity + 1, 100)).has_value());
  EXPECT_TRUE(validationError(OrderRequest::limit("AAPL", Side::Buy, 1, kMaxPrice + 1)).has_value());
  EXPECT_TRUE(
      validationError(OrderRequest::limit("AAPL", Side::Buy, 1, 100, TimeInForce::GTC, "a b"))
          .has_value());
}

TEST(ExecutionReport, AverageFillPriceIsVolumeWeighted) {
  ExecutionReport report;
  EXPECT_DOUBLE_EQ(report.averageFillPrice(), 0.0);
  Trade a;
  a.price = 10000;
  a.quantity = 1;
  Trade b;
  b.price = 10300;
  b.quantity = 3;
  report.trades = {a, b};
  EXPECT_DOUBLE_EQ(report.averageFillPrice(), 102.25);
}
