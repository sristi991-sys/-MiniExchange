#include "mx/matching/MatchingEngine.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace mx;

namespace {

class MatchingEngineTest : public ::testing::Test {
 protected:
  ExecutionReport limit(Side side, Price price, Quantity quantity,
                        TimeInForce tif = TimeInForce::GTC, std::string trader = {}) {
    return engine.process(
        Order(nextId++, "TEST", side, OrderType::Limit, tif, price, quantity, std::move(trader)));
  }

  ExecutionReport market(Side side, Quantity quantity, TimeInForce tif = TimeInForce::IOC) {
    return engine.process(Order(nextId++, "TEST", side, OrderType::Market, tif, 0, quantity));
  }

  const OrderBook& book() const { return engine.book(); }

  static void expectConserved(const ExecutionReport& r) {
    EXPECT_EQ(r.quantity, r.filledQuantity + r.restingQuantity + r.cancelledQuantity)
        << "order #" << r.orderId;
  }

  MatchingEngine engine{"TEST"};
  OrderId nextId = 1;
};

}  // namespace

TEST_F(MatchingEngineTest, NonCrossingOrdersRest) {
  const auto bid = limit(Side::Buy, 100, 10);
  const auto ask = limit(Side::Sell, 101, 10);
  EXPECT_TRUE(bid.trades.empty());
  EXPECT_TRUE(ask.trades.empty());
  EXPECT_EQ(bid.status, OrderStatus::New);
  EXPECT_EQ(bid.restingQuantity, 10u);
  EXPECT_EQ(book().bestBid(), Price{100});
  EXPECT_EQ(book().bestAsk(), Price{101});
  expectConserved(bid);
}

TEST_F(MatchingEngineTest, ExactMatchFillsBothOrders) {
  limit(Side::Sell, 100, 10);
  const auto buy = limit(Side::Buy, 100, 10);
  ASSERT_EQ(buy.trades.size(), 1u);
  EXPECT_EQ(buy.trades[0].price, 100);
  EXPECT_EQ(buy.trades[0].quantity, 10u);
  EXPECT_EQ(buy.trades[0].buyOrderId, 2u);
  EXPECT_EQ(buy.trades[0].sellOrderId, 1u);
  EXPECT_EQ(buy.status, OrderStatus::Filled);
  EXPECT_TRUE(book().empty());
  expectConserved(buy);
}

TEST_F(MatchingEngineTest, TradesExecuteAtRestingPrice) {
  limit(Side::Sell, 100, 5);
  const auto buy = limit(Side::Buy, 105, 5);  // Willing to pay more; gets 100.
  ASSERT_EQ(buy.trades.size(), 1u);
  EXPECT_EQ(buy.trades[0].price, 100);

  limit(Side::Buy, 90, 5);
  const auto sell = limit(Side::Sell, 80, 5);  // Willing to sell lower; gets 90.
  ASSERT_EQ(sell.trades.size(), 1u);
  EXPECT_EQ(sell.trades[0].price, 90);
}

TEST_F(MatchingEngineTest, PartiallyFilledIncomingOrderRestsRemainder) {
  limit(Side::Sell, 100, 30);
  const auto buy = limit(Side::Buy, 100, 100);
  EXPECT_EQ(buy.status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(buy.filledQuantity, 30u);
  EXPECT_EQ(buy.restingQuantity, 70u);
  EXPECT_EQ(book().bestBid(), Price{100});
  EXPECT_EQ(book().volumeAt(Side::Buy, 100), 70u);
  EXPECT_FALSE(book().bestAsk().has_value());
  EXPECT_EQ(book().find(buy.orderId)->status(), OrderStatus::PartiallyFilled);
  expectConserved(buy);
}

TEST_F(MatchingEngineTest, RestingOrderCanBePartiallyFilled) {
  const auto sell = limit(Side::Sell, 100, 100);
  const auto buy = limit(Side::Buy, 100, 40);
  EXPECT_EQ(buy.status, OrderStatus::Filled);
  const auto resting = book().find(sell.orderId);
  ASSERT_TRUE(resting.has_value());
  EXPECT_EQ(resting->status(), OrderStatus::PartiallyFilled);
  EXPECT_EQ(resting->remainingQuantity(), 60u);
  EXPECT_EQ(book().volumeAt(Side::Sell, 100), 60u);
}

TEST_F(MatchingEngineTest, BetterPriceHasPriority) {
  const auto at102 = limit(Side::Sell, 102, 10);
  const auto at100 = limit(Side::Sell, 100, 10);
  const auto at101 = limit(Side::Sell, 101, 10);
  const auto buy = limit(Side::Buy, 102, 30);
  ASSERT_EQ(buy.trades.size(), 3u);
  EXPECT_EQ(buy.trades[0].sellOrderId, at100.orderId);
  EXPECT_EQ(buy.trades[1].sellOrderId, at101.orderId);
  EXPECT_EQ(buy.trades[2].sellOrderId, at102.orderId);
  EXPECT_EQ(buy.trades[0].price, 100);
  EXPECT_EQ(buy.trades[1].price, 101);
  EXPECT_EQ(buy.trades[2].price, 102);
}

TEST_F(MatchingEngineTest, EarlierOrderHasPriorityAtSamePrice) {
  const auto first = limit(Side::Sell, 100, 10);
  const auto second = limit(Side::Sell, 100, 10);
  const auto third = limit(Side::Sell, 100, 10);
  const auto buy = limit(Side::Buy, 100, 15);
  ASSERT_EQ(buy.trades.size(), 2u);
  EXPECT_EQ(buy.trades[0].sellOrderId, first.orderId);
  EXPECT_EQ(buy.trades[0].quantity, 10u);
  EXPECT_EQ(buy.trades[1].sellOrderId, second.orderId);
  EXPECT_EQ(buy.trades[1].quantity, 5u);

  const auto remaining = book().orders(Side::Sell);
  ASSERT_EQ(remaining.size(), 2u);
  EXPECT_EQ(remaining[0].id(), second.orderId);  // Partially filled order keeps its place.
  EXPECT_EQ(remaining[0].remainingQuantity(), 5u);
  EXPECT_EQ(remaining[1].id(), third.orderId);
}

TEST_F(MatchingEngineTest, SweepStopsAtLimitPrice) {
  limit(Side::Sell, 100, 10);
  limit(Side::Sell, 101, 10);
  limit(Side::Sell, 102, 10);
  const auto buy = limit(Side::Buy, 101, 25);
  ASSERT_EQ(buy.trades.size(), 2u);
  EXPECT_EQ(buy.filledQuantity, 20u);
  EXPECT_EQ(buy.restingQuantity, 5u);
  EXPECT_EQ(book().bestBid(), Price{101});
  EXPECT_EQ(book().bestAsk(), Price{102});
  EXPECT_EQ(book().volumeAt(Side::Sell, 102), 10u);
}

TEST_F(MatchingEngineTest, SellAggressorIsSymmetric) {
  const auto bid100 = limit(Side::Buy, 100, 10, TimeInForce::GTC, "alice");
  const auto bid99 = limit(Side::Buy, 99, 10, TimeInForce::GTC, "bob");
  const auto sell = limit(Side::Sell, 99, 15, TimeInForce::GTC, "carol");
  ASSERT_EQ(sell.trades.size(), 2u);
  EXPECT_EQ(sell.trades[0].buyOrderId, bid100.orderId);
  EXPECT_EQ(sell.trades[0].price, 100);
  EXPECT_EQ(sell.trades[1].buyOrderId, bid99.orderId);
  EXPECT_EQ(sell.trades[1].quantity, 5u);
  for (const Trade& trade : sell.trades) {
    EXPECT_EQ(trade.aggressorSide, Side::Sell);
    EXPECT_EQ(trade.sellOrderId, sell.orderId);
    EXPECT_EQ(trade.sellTrader, "carol");
  }
  EXPECT_EQ(sell.trades[0].buyTrader, "alice");
  EXPECT_EQ(sell.trades[1].buyTrader, "bob");
  EXPECT_EQ(book().volumeAt(Side::Buy, 99), 5u);
}

TEST_F(MatchingEngineTest, MarketOrderWalksTheBook) {
  limit(Side::Sell, 100, 10);
  limit(Side::Sell, 105, 10);
  const auto buy = market(Side::Buy, 15);
  ASSERT_EQ(buy.trades.size(), 2u);
  EXPECT_EQ(buy.trades[0].price, 100);
  EXPECT_EQ(buy.trades[1].price, 105);
  EXPECT_EQ(buy.status, OrderStatus::Filled);
  EXPECT_EQ(book().volumeAt(Side::Sell, 105), 5u);
  EXPECT_DOUBLE_EQ(buy.averageFillPrice(), (100.0 * 10 + 105.0 * 5) / 15 / 100);
}

TEST_F(MatchingEngineTest, MarketOrderRemainderIsCancelledNotRested) {
  limit(Side::Sell, 100, 10);
  const auto buy = market(Side::Buy, 25);
  EXPECT_EQ(buy.filledQuantity, 10u);
  EXPECT_EQ(buy.cancelledQuantity, 15u);
  EXPECT_EQ(buy.restingQuantity, 0u);
  EXPECT_EQ(buy.status, OrderStatus::Cancelled);
  EXPECT_FALSE(buy.message.empty());
  EXPECT_TRUE(book().empty());
  expectConserved(buy);
}

TEST_F(MatchingEngineTest, MarketOrderOnEmptyBook) {
  const auto sell = market(Side::Sell, 10);
  EXPECT_TRUE(sell.trades.empty());
  EXPECT_EQ(sell.status, OrderStatus::Cancelled);
  EXPECT_EQ(sell.cancelledQuantity, 10u);
  EXPECT_TRUE(book().empty());
}

TEST_F(MatchingEngineTest, IocFillsWhatItCanAndCancelsTheRest) {
  limit(Side::Sell, 100, 10);
  const auto buy = limit(Side::Buy, 100, 20, TimeInForce::IOC);
  EXPECT_EQ(buy.filledQuantity, 10u);
  EXPECT_EQ(buy.cancelledQuantity, 10u);
  EXPECT_EQ(buy.status, OrderStatus::Cancelled);
  EXPECT_FALSE(book().bestBid().has_value());
  expectConserved(buy);
}

TEST_F(MatchingEngineTest, IocWithoutCrossDoesNotRest) {
  limit(Side::Sell, 105, 10);
  const auto buy = limit(Side::Buy, 100, 10, TimeInForce::IOC);
  EXPECT_TRUE(buy.trades.empty());
  EXPECT_EQ(buy.status, OrderStatus::Cancelled);
  EXPECT_EQ(book().orderCount(), 1u);
}

TEST_F(MatchingEngineTest, FokWithInsufficientLiquidityLeavesBookUntouched) {
  limit(Side::Sell, 100, 10);
  limit(Side::Sell, 101, 10);
  const auto buy = limit(Side::Buy, 101, 25, TimeInForce::FOK);
  EXPECT_TRUE(buy.trades.empty());
  EXPECT_EQ(buy.status, OrderStatus::Cancelled);
  EXPECT_EQ(buy.cancelledQuantity, 25u);
  EXPECT_EQ(book().volumeAt(Side::Sell, 100), 10u);
  EXPECT_EQ(book().volumeAt(Side::Sell, 101), 10u);
  expectConserved(buy);
}

TEST_F(MatchingEngineTest, FokFillsCompletelyWhenPossible) {
  limit(Side::Sell, 100, 10);
  limit(Side::Sell, 101, 10);
  const auto buy = limit(Side::Buy, 101, 20, TimeInForce::FOK);
  EXPECT_EQ(buy.status, OrderStatus::Filled);
  EXPECT_EQ(buy.trades.size(), 2u);
  EXPECT_TRUE(book().empty());
}

TEST_F(MatchingEngineTest, FokOnlyCountsLiquidityWithinLimit) {
  limit(Side::Sell, 100, 10);
  limit(Side::Sell, 105, 20);
  const auto buy = limit(Side::Buy, 101, 20, TimeInForce::FOK);
  EXPECT_EQ(buy.status, OrderStatus::Cancelled);
  EXPECT_TRUE(buy.trades.empty());
  EXPECT_EQ(book().orderCount(), 2u);

  const auto marketFok = market(Side::Buy, 30, TimeInForce::FOK);  // Market FOK: any price.
  EXPECT_EQ(marketFok.status, OrderStatus::Filled);
}

TEST_F(MatchingEngineTest, CancelRestingOrder) {
  const auto bid = limit(Side::Buy, 100, 10);
  const CancelResult result = engine.cancel(bid.orderId);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.cancelledQuantity, 10u);
  EXPECT_EQ(result.symbol, "TEST");
  EXPECT_TRUE(book().empty());
  EXPECT_FALSE(engine.cancel(bid.orderId).success);  // Second cancel fails.
}

TEST_F(MatchingEngineTest, CancelPartiallyFilledOrderReportsRemainder) {
  const auto sell = limit(Side::Sell, 100, 100);
  limit(Side::Buy, 100, 40);
  const CancelResult result = engine.cancel(sell.orderId);
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.cancelledQuantity, 60u);
}

TEST_F(MatchingEngineTest, CannotCancelFilledOrUnknownOrders) {
  const auto sell = limit(Side::Sell, 100, 10);
  limit(Side::Buy, 100, 10);
  EXPECT_FALSE(engine.cancel(sell.orderId).success);
  EXPECT_FALSE(engine.cancel(9999).success);
}

TEST_F(MatchingEngineTest, RejectsInvalidOrders) {
  const auto wrongSymbol =
      engine.process(Order(nextId++, "OTHER", Side::Buy, OrderType::Limit, TimeInForce::GTC, 1, 1));
  EXPECT_EQ(wrongSymbol.status, OrderStatus::Rejected);

  const auto resting = limit(Side::Buy, 100, 1);
  const auto duplicate = engine.process(
      Order(resting.orderId, "TEST", Side::Sell, OrderType::Limit, TimeInForce::GTC, 200, 1));
  EXPECT_EQ(duplicate.status, OrderStatus::Rejected);

  const auto zeroPrice = limit(Side::Buy, 0, 5);
  EXPECT_EQ(zeroPrice.status, OrderStatus::Rejected);
  const auto zeroQty = limit(Side::Buy, 10, 0);
  EXPECT_EQ(zeroQty.status, OrderStatus::Rejected);

  EXPECT_EQ(book().orderCount(), 1u);
}

TEST_F(MatchingEngineTest, TradeIdsAreSequentialAndShared) {
  IdGenerator shared(500);
  MatchingEngine a("AAA", shared);
  MatchingEngine b("BBB", shared);
  (void)a.process(Order(1, "AAA", Side::Sell, OrderType::Limit, TimeInForce::GTC, 10, 5));
  (void)b.process(Order(2, "BBB", Side::Sell, OrderType::Limit, TimeInForce::GTC, 10, 5));
  const auto t1 = a.process(Order(3, "AAA", Side::Buy, OrderType::Limit, TimeInForce::GTC, 10, 5));
  const auto t2 = b.process(Order(4, "BBB", Side::Buy, OrderType::Limit, TimeInForce::GTC, 10, 5));
  ASSERT_EQ(t1.trades.size(), 1u);
  ASSERT_EQ(t2.trades.size(), 1u);
  EXPECT_EQ(t1.trades[0].id, 500u);
  EXPECT_EQ(t2.trades[0].id, 501u);
  EXPECT_EQ(a.tradesExecuted(), 1u);
  EXPECT_EQ(a.ordersProcessed(), 2u);
}

TEST_F(MatchingEngineTest, TradesCarryTimestampsAndSymbol) {
  limit(Side::Sell, 100, 1);
  const auto buy = limit(Side::Buy, 100, 1);  // No timestamp: falls back to the clock.
  ASSERT_EQ(buy.trades.size(), 1u);
  EXPECT_GT(buy.trades[0].timestamp, 0);
  EXPECT_EQ(buy.trades[0].symbol, "TEST");

  limit(Side::Sell, 100, 1);
  const auto stamped = engine.process(
      Order(nextId++, "TEST", Side::Buy, OrderType::Limit, TimeInForce::GTC, 100, 1, "", 777));
  ASSERT_EQ(stamped.trades.size(), 1u);
  EXPECT_EQ(stamped.trades[0].timestamp, 777);  // Aggressor's arrival time.
}
