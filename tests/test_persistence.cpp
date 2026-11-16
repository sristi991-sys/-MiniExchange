#include "mx/exchange/Exchange.hpp"
#include "mx/persistence/EventJournal.hpp"
#include "mx/persistence/TradeStore.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mx;
namespace fs = std::filesystem;

namespace {

/// Gives each test its own scratch directory and removes it afterwards (RAII).
class PersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() /
           ("mx_test_" + std::string(info->name()) + "_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  std::string path(const std::string& name) const { return (dir_ / name).string(); }

  fs::path dir_;
};

Trade sampleTrade(TradeId id) {
  Trade t;
  t.id = id;
  t.symbol = "AAPL";
  t.price = 15025 + static_cast<Price>(id);
  t.quantity = 10 * id;
  t.buyOrderId = 100 + id;
  t.sellOrderId = 200 + id;
  t.buyTrader = "alice";
  t.sellTrader = id % 2 == 0 ? "" : "bob";
  t.aggressorSide = id % 2 == 0 ? Side::Buy : Side::Sell;
  t.timestamp = 1'700'000'000'000'000'000LL + static_cast<Timestamp>(id);
  return t;
}

}  // namespace

TEST_F(PersistenceTest, TradeCsvRowRoundTrip) {
  const Trade original = sampleTrade(3);
  const auto parsed = parseTradeCsvRow(toCsvRow(original));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, original);
  EXPECT_FALSE(parseTradeCsvRow("1,2,3").has_value());
  EXPECT_FALSE(parseTradeCsvRow("x,1,AAPL,1.00,1,1,2,a,b,BUY").has_value());
}

TEST_F(PersistenceTest, TradeCsvFileRoundTrip) {
  std::vector<Trade> trades;
  for (TradeId id = 1; id <= 50; ++id) {
    trades.push_back(sampleTrade(id));
  }
  const std::string file = path("trades.csv");
  writeTradesCsv(file, trades);
  EXPECT_EQ(readTradesCsv(file), trades);

  std::ifstream in(file);
  std::string header;
  std::getline(in, header);
  EXPECT_EQ(header, tradeCsvHeader());
}

TEST_F(PersistenceTest, TradeCsvErrors) {
  EXPECT_THROW((void)readTradesCsv(path("missing.csv")), std::runtime_error);
  const std::string file = path("bad.csv");
  {
    std::ofstream out(file);
    out << tradeCsvHeader() << "\nnot,a,trade\n";
  }
  EXPECT_THROW((void)readTradesCsv(file), std::runtime_error);
}

TEST_F(PersistenceTest, JournalEncodeDecodeRoundTrip) {
  const Order order(42, "MSFT", Side::Sell, OrderType::Limit, TimeInForce::IOC, 30050, 75, "bob",
                    123456789);
  const auto decoded = EventJournal::decode(EventJournal::encodeNewOrder(order));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->type, JournalEventType::NewOrder);
  EXPECT_EQ(decoded->order.id(), 42u);
  EXPECT_EQ(decoded->order.side(), Side::Sell);
  EXPECT_EQ(decoded->order.timeInForce(), TimeInForce::IOC);
  EXPECT_EQ(decoded->order.price(), 30050);
  EXPECT_EQ(decoded->order.quantity(), 75u);
  EXPECT_EQ(decoded->order.trader(), "bob");
  EXPECT_EQ(decoded->order.timestamp(), 123456789);

  const auto cancel = EventJournal::decode(EventJournal::encodeCancel("MSFT", 42, 5));
  ASSERT_TRUE(cancel.has_value());
  EXPECT_EQ(cancel->type, JournalEventType::CancelOrder);
  EXPECT_EQ(cancel->orderId, 42u);

  const auto symbol = EventJournal::decode(EventJournal::encodeAddSymbol("TSLA"));
  ASSERT_TRUE(symbol.has_value());
  EXPECT_EQ(symbol->symbol, "TSLA");

  EXPECT_FALSE(EventJournal::decode("N,1,2,AAPL,BUY").has_value());
  EXPECT_FALSE(EventJournal::decode("X,1").has_value());
}

TEST_F(PersistenceTest, JournalSkipsCommentsAndCountsMalformedLines) {
  const std::string file = path("journal.log");
  {
    EventJournal journal(file);
    journal.appendAddSymbol("AAPL");
    journal.appendNewOrder(
        Order(1, "AAPL", Side::Buy, OrderType::Limit, TimeInForce::GTC, 100, 10, "", 1));
    journal.appendCancel("AAPL", 1, 2);
    EXPECT_EQ(journal.eventsWritten(), 3u);
  }
  {
    std::ofstream out(file, std::ios::app);
    out << "N,2,3,AAPL,BU";  // Torn write, as after a crash.
  }
  const JournalReadResult result = EventJournal::read(file);
  EXPECT_EQ(result.events.size(), 3u);
  EXPECT_EQ(result.malformedLines, 1u);
  EXPECT_THROW((void)EventJournal::read(path("missing.log")), std::runtime_error);
}

TEST_F(PersistenceTest, RecoveryRebuildsIdenticalState) {
  const std::string file = path("exchange.journal");
  std::vector<Trade> originalTrades;
  std::vector<BookSnapshot> originalBooks;
  OrderId lastId = 0;
  {
    ExchangeConfig config;
    config.journalPath = file;
    Exchange exchange(config);
    exchange.addSymbol("AAA");
    exchange.addSymbol("BBB");

    std::mt19937 rng(2024);
    std::vector<std::pair<std::string, OrderId>> placed;
    for (int i = 0; i < 2000; ++i) {
      const std::string symbol = rng() % 2 == 0 ? "AAA" : "BBB";
      if (rng() % 5 == 0 && !placed.empty()) {
        const auto& [sym, id] = placed[rng() % placed.size()];
        exchange.cancelOrder(sym, id);
        continue;
      }
      const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
      const Price price = 1000 + static_cast<Price>(rng() % 20);
      const Quantity qty = 1 + rng() % 40;
      const ExecutionReport report =
          rng() % 10 == 0
              ? exchange.submitOrder(OrderRequest::market(symbol, side, qty, "mkt"))
              : exchange.submitOrder(OrderRequest::limit(symbol, side, qty, price,
                                                         TimeInForce::GTC, "t" + std::to_string(i % 7)));
      placed.emplace_back(symbol, report.orderId);
      lastId = report.orderId;
    }
    exchange.submitOrder(OrderRequest::limit("ZZZ", Side::Buy, 1, 1));  // Rejected: not journaled.
    originalTrades = exchange.trades().allTrades();
    originalBooks = {*exchange.snapshot("AAA"), *exchange.snapshot("BBB")};
    ASSERT_FALSE(originalTrades.empty());
  }

  Exchange recovered;
  const RecoveryStats stats = recovered.recover(file);
  EXPECT_EQ(stats.symbolsAdded, 2u);
  EXPECT_GT(stats.ordersReplayed, 1000u);
  EXPECT_GT(stats.cancelsReplayed, 0u);
  EXPECT_EQ(stats.tradesReplayed, originalTrades.size());
  EXPECT_EQ(stats.malformedLines, 0u);

  EXPECT_EQ(recovered.snapshot("AAA")->bids, originalBooks[0].bids);
  EXPECT_EQ(recovered.snapshot("AAA")->asks, originalBooks[0].asks);
  EXPECT_EQ(recovered.snapshot("BBB")->bids, originalBooks[1].bids);
  EXPECT_EQ(recovered.snapshot("BBB")->asks, originalBooks[1].asks);
  // Bit-for-bit identical, timestamps included (trades use the journaled arrival time).
  EXPECT_EQ(recovered.trades().allTrades(), originalTrades);

  // New orders must never reuse a recovered id.
  const ExecutionReport next =
      recovered.submitOrder(OrderRequest::limit("AAA", Side::Buy, 1, 1));
  EXPECT_GT(next.orderId, lastId);
}

TEST_F(PersistenceTest, JournalAppendsAcrossSessions) {
  const std::string file = path("session.journal");
  {
    ExchangeConfig config;
    config.journalPath = file;
    Exchange exchange(config);
    exchange.addSymbol("AAA");
    exchange.submitOrder(OrderRequest::limit("AAA", Side::Sell, 10, 500));
  }
  {
    ExchangeConfig config;
    config.journalPath = file;
    Exchange exchange(config);
    exchange.recover(file);
    EXPECT_FALSE(exchange.addSymbol("AAA"));  // Already recovered; not re-journaled.
    exchange.submitOrder(OrderRequest::limit("AAA", Side::Buy, 4, 500));
  }
  Exchange third;
  third.recover(file);
  const auto quote = third.quote("AAA");
  ASSERT_TRUE(quote.has_value());
  EXPECT_EQ(quote->askQuantity, 6u);
  EXPECT_EQ(third.trades().tradeCount(), 1u);
}
