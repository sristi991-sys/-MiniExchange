#include "mx/exchange/Exchange.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

using namespace mx;

namespace {

class ExchangeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    exchange.addSymbol("AAPL");
    exchange.addSymbol("MSFT");
  }

  ExecutionReport buy(const std::string& symbol, Quantity qty, Price price,
                      std::string trader = {}) {
    return exchange.submitOrder(
        OrderRequest::limit(symbol, Side::Buy, qty, price, TimeInForce::GTC, std::move(trader)));
  }
  ExecutionReport sell(const std::string& symbol, Quantity qty, Price price,
                       std::string trader = {}) {
    return exchange.submitOrder(
        OrderRequest::limit(symbol, Side::Sell, qty, price, TimeInForce::GTC, std::move(trader)));
  }

  Exchange exchange;
};

}  // namespace

TEST_F(ExchangeTest, ManagesSymbols) {
  EXPECT_TRUE(exchange.hasSymbol("AAPL"));
  EXPECT_FALSE(exchange.hasSymbol("TSLA"));
  EXPECT_FALSE(exchange.addSymbol("AAPL"));  // duplicate
  EXPECT_FALSE(exchange.addSymbol("bad symbol"));
  EXPECT_TRUE(exchange.addSymbol("TSLA"));
  EXPECT_EQ(exchange.symbols(), (std::vector<std::string>{"AAPL", "MSFT", "TSLA"}));
}

TEST_F(ExchangeTest, RejectsUnknownSymbolsAndInvalidRequests) {
  const auto unknown = buy("TSLA", 10, 100);
  EXPECT_EQ(unknown.status, OrderStatus::Rejected);
  EXPECT_NE(unknown.message.find("unknown symbol"), std::string::npos);

  EXPECT_EQ(buy("AAPL", 0, 100).status, OrderStatus::Rejected);
  EXPECT_EQ(buy("AAPL", 10, 0).status, OrderStatus::Rejected);
  EXPECT_EQ(buy("AAPL", 10, 100, "has space").status, OrderStatus::Rejected);
  EXPECT_EQ(exchange.restingOrderCount(), 0u);

  const MetricsSnapshot metrics = exchange.metrics().snapshot();
  EXPECT_EQ(metrics.ordersRejected, 4u);
  EXPECT_EQ(metrics.ordersAccepted, 0u);
}

TEST_F(ExchangeTest, AssignsUniqueIncreasingOrderIds) {
  OrderId previous = 0;
  for (int i = 0; i < 20; ++i) {
    const auto report = buy(i % 2 == 0 ? "AAPL" : "MSFT", 1, 100 + i);
    EXPECT_GT(report.orderId, previous);
    previous = report.orderId;
  }
}

TEST_F(ExchangeTest, SymbolsAreIsolated) {
  sell("AAPL", 10, 100);
  const auto msftBuy = buy("MSFT", 10, 100);
  EXPECT_TRUE(msftBuy.trades.empty());
  EXPECT_EQ(exchange.quote("AAPL")->askPrice, Price{100});
  EXPECT_EQ(exchange.quote("MSFT")->bidPrice, Price{100});
}

TEST_F(ExchangeTest, MatchesAndRecordsTrades) {
  sell("AAPL", 50, 15025, "carol");
  sell("AAPL", 70, 15025, "dave");
  sell("AAPL", 100, 15050, "bob");
  const auto report = buy("AAPL", 130, 15050, "alice");

  EXPECT_EQ(report.status, OrderStatus::Filled);
  ASSERT_EQ(report.trades.size(), 3u);
  EXPECT_EQ(report.trades[0].sellTrader, "carol");
  EXPECT_EQ(report.trades[1].sellTrader, "dave");
  EXPECT_EQ(report.trades[2].sellTrader, "bob");
  EXPECT_EQ(report.trades[2].quantity, 10u);

  const auto stats = exchange.trades().statistics("AAPL");
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->tradeCount, 3u);
  EXPECT_EQ(stats->volume, 130u);
  EXPECT_EQ(stats->lastPrice, 15050);
  EXPECT_EQ(exchange.trades().tradesForOrder(report.orderId).size(), 3u);

  const auto quote = exchange.quote("AAPL");
  EXPECT_EQ(quote->askPrice, Price{15050});
  EXPECT_EQ(quote->askQuantity, 90u);
  EXPECT_FALSE(quote->bidPrice.has_value());

  const MetricsSnapshot metrics = exchange.metrics().snapshot();
  EXPECT_EQ(metrics.ordersAccepted, 4u);
  EXPECT_EQ(metrics.trades, 3u);
  EXPECT_EQ(metrics.volume, 130u);
}

TEST_F(ExchangeTest, CancelBySymbolOrByIdAlone) {
  const auto a = buy("AAPL", 10, 100);
  const auto m = buy("MSFT", 20, 200);

  const auto bySymbol = exchange.cancelOrder("AAPL", a.orderId);
  EXPECT_TRUE(bySymbol.success);
  EXPECT_EQ(bySymbol.cancelledQuantity, 10u);

  const auto wrongSymbol = exchange.cancelOrder("AAPL", m.orderId);
  EXPECT_FALSE(wrongSymbol.success);

  const auto byId = exchange.cancelOrder(m.orderId);
  EXPECT_TRUE(byId.success);
  EXPECT_EQ(byId.symbol, "MSFT");
  EXPECT_FALSE(exchange.cancelOrder(m.orderId).success);
  EXPECT_FALSE(exchange.cancelOrder("NOPE", 1).success);
  EXPECT_EQ(exchange.restingOrderCount(), 0u);
}

TEST_F(ExchangeTest, QueriesReturnConsistentSnapshots) {
  buy("AAPL", 10, 100);
  buy("AAPL", 5, 100);
  buy("AAPL", 7, 99);
  sell("AAPL", 3, 105);

  const auto snap = exchange.snapshot("AAPL", 1);
  ASSERT_TRUE(snap.has_value());
  ASSERT_EQ(snap->bids.size(), 1u);
  EXPECT_EQ(snap->bids[0], (LevelInfo{100, 15, 2}));
  EXPECT_EQ(exchange.restingOrders("AAPL", Side::Buy).size(), 3u);
  EXPECT_FALSE(exchange.snapshot("NOPE").has_value());
  EXPECT_FALSE(exchange.quote("NOPE").has_value());
  EXPECT_TRUE(exchange.restingOrders("NOPE", Side::Buy).empty());
  const auto found = exchange.findOrder("AAPL", 1);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->quantity(), 10u);
}

TEST_F(ExchangeTest, ListenersRunOutsideTheBookLock) {
  std::atomic<int> notifications{0};
  // Re-entering the exchange from a listener would deadlock if listeners ran
  // while the symbol's mutex was held.
  exchange.trades().subscribe([this, &notifications](const Trade& trade) {
    const auto q = exchange.quote(trade.symbol);
    EXPECT_TRUE(q.has_value());
    notifications.fetch_add(1);
  });
  sell("AAPL", 10, 100);
  buy("AAPL", 10, 100);
  EXPECT_EQ(notifications.load(), 1);
}

TEST(ExchangeConfigTest, HistoryAndMetricsCanBeDisabled) {
  ExchangeConfig config;
  config.storeTradeHistory = false;
  config.enableMetrics = false;
  Exchange exchange(config);
  exchange.addSymbol("AAA");
  exchange.submitOrder(OrderRequest::limit("AAA", Side::Sell, 5, 100));
  exchange.submitOrder(OrderRequest::limit("AAA", Side::Buy, 5, 100));
  EXPECT_TRUE(exchange.trades().allTrades().empty());
  EXPECT_EQ(exchange.trades().statistics("AAA")->volume, 5u);
  EXPECT_EQ(exchange.metrics().snapshot().ordersReceived, 0u);
}
