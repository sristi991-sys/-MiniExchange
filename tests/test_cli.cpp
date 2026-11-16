#include "CommandProcessor.hpp"

#include <gtest/gtest.h>

#include <sstream>

using namespace mx;

namespace {

class CliTest : public ::testing::Test {
 protected:
  void SetUp() override { exchange.addSymbol("AAPL"); }

  std::string run(const std::string& line) {
    out.str("");
    processor.execute(line);
    return out.str();
  }

  Exchange exchange;
  std::ostringstream out;
  cli::CommandProcessor processor{exchange, out};
};

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_F(CliTest, PlacesAndMatchesOrders) {
  EXPECT_TRUE(contains(run("sell aapl 100 150.25 bob"), "Order #1 SELL 100 AAPL @150.25 GTC -> NEW"));
  const std::string fill = run("BUY AAPL 40 151 alice");
  EXPECT_TRUE(contains(fill, "FILLED"));
  EXPECT_TRUE(contains(fill, "40 @ 150.25"));
  EXPECT_TRUE(contains(fill, "alice"));
}

TEST_F(CliTest, MarketAndTimeInForceOptions) {
  run("sell AAPL 10 100");
  const std::string market = run("buy AAPL 25 MKT");
  EXPECT_TRUE(contains(market, "MKT IOC -> CANCELLED"));
  EXPECT_TRUE(contains(market, "cancelled 15"));
  EXPECT_TRUE(contains(run("buy AAPL 5 99 FOK"), "FOK -> CANCELLED"));
}

TEST_F(CliTest, ShowsBookQuoteOrdersAndTrades) {
  run("buy AAPL 10 99.50");
  run("sell AAPL 7 100.25");
  const std::string book = run("book AAPL");
  EXPECT_TRUE(contains(book, "99.50"));
  EXPECT_TRUE(contains(book, "100.25"));
  EXPECT_TRUE(contains(book, "spread 0.75"));
  EXPECT_TRUE(contains(run("quote AAPL"), "bid 99.50 x 10"));
  EXPECT_TRUE(contains(run("orders AAPL"), "#1 BUY 10 AAPL"));
  EXPECT_TRUE(contains(run("trades"), "No trades"));
  run("sell AAPL 10 99.50");
  EXPECT_TRUE(contains(run("trades AAPL"), "10 @ 99.50"));
  EXPECT_TRUE(contains(run("stats"), "AAPL"));
  EXPECT_TRUE(contains(run("metrics"), "orders received: 3"));
}

TEST_F(CliTest, CancelsOrders) {
  run("buy AAPL 10 99");
  EXPECT_TRUE(contains(run("cancel 1"), "Cancelled order #1"));
  EXPECT_TRUE(contains(run("cancel 1"), "Cancel failed"));
  run("buy AAPL 10 99");
  EXPECT_TRUE(contains(run("cancel 2 AAPL"), "Cancelled order #2"));
}

TEST_F(CliTest, ReportsErrorsHelpfully) {
  EXPECT_TRUE(contains(run("buy AAPL ten 100"), "quantity must be a positive whole number"));
  EXPECT_TRUE(contains(run("buy AAPL 10 abc"), "invalid price"));
  EXPECT_TRUE(contains(run("buy AAPL 10"), "Usage"));
  EXPECT_TRUE(contains(run("buy NOPE 10 100"), "REJECTED: unknown symbol"));
  EXPECT_TRUE(contains(run("frobnicate"), "Unknown command"));
  EXPECT_TRUE(contains(run("book NOPE"), "Unknown symbol"));
  EXPECT_TRUE(contains(run("cancel x"), "invalid order id"));
  EXPECT_TRUE(run("# a comment").empty());
  EXPECT_TRUE(run("   ").empty());
}

TEST_F(CliTest, SymbolManagementAndQuit) {
  EXPECT_TRUE(contains(run("addsymbol msft"), "Added symbol MSFT"));
  EXPECT_TRUE(contains(run("addsymbol MSFT"), "Cannot add"));
  EXPECT_TRUE(contains(run("symbols"), "AAPL MSFT"));
  EXPECT_TRUE(contains(run("help"), "buy  <SYMBOL>"));
  EXPECT_FALSE(processor.execute("quit"));
  EXPECT_TRUE(processor.execute("symbols"));
}
