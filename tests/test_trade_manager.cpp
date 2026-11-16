#include "mx/trade/TradeManager.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

using namespace mx;

namespace {

Trade makeTrade(TradeId id, std::string symbol, Price price, Quantity quantity, OrderId buy = 1,
                OrderId sell = 2) {
  Trade t;
  t.id = id;
  t.symbol = std::move(symbol);
  t.price = price;
  t.quantity = quantity;
  t.buyOrderId = buy;
  t.sellOrderId = sell;
  return t;
}

}  // namespace

TEST(TradeManager, RecordsAndQueriesTrades) {
  TradeManager manager;
  manager.record(makeTrade(2, "BBB", 200, 5, 10, 11));
  manager.record(makeTrade(1, "AAA", 100, 10, 12, 13));
  manager.record(makeTrade(3, "AAA", 102, 20, 12, 14));

  const auto all = manager.allTrades();
  ASSERT_EQ(all.size(), 3u);
  EXPECT_EQ(all[0].id, 1u);  // Sorted by trade id across symbols.
  EXPECT_EQ(all[2].id, 3u);

  EXPECT_EQ(manager.tradesForSymbol("AAA").size(), 2u);
  EXPECT_TRUE(manager.tradesForSymbol("ZZZ").empty());
  EXPECT_EQ(manager.tradesForOrder(12).size(), 2u);
  EXPECT_EQ(manager.tradesForOrder(11).size(), 1u);
  EXPECT_EQ(manager.recentTrades(2).front().id, 2u);
  EXPECT_EQ(manager.tradeCount(), 3u);
  EXPECT_EQ(manager.totalVolume(), 35u);
}

TEST(TradeManager, ComputesSymbolStatistics) {
  TradeManager manager;
  manager.record(makeTrade(1, "AAA", 10000, 10));
  manager.record(makeTrade(2, "AAA", 10200, 30));
  manager.record(makeTrade(3, "AAA", 9900, 10));

  const auto stats = manager.statistics("AAA");
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->tradeCount, 3u);
  EXPECT_EQ(stats->volume, 50u);
  EXPECT_EQ(stats->openPrice, 10000);
  EXPECT_EQ(stats->lastPrice, 9900);
  EXPECT_EQ(stats->highPrice, 10200);
  EXPECT_EQ(stats->lowPrice, 9900);
  EXPECT_DOUBLE_EQ(stats->vwap(), (100.0 * 10 + 102.0 * 30 + 99.0 * 10) / 50);
  EXPECT_FALSE(manager.statistics("NONE").has_value());
}

TEST(TradeManager, StatisticsOnlyModeDropsHistory) {
  TradeManager manager(/*storeHistory=*/false);
  manager.record(makeTrade(1, "AAA", 100, 10));
  EXPECT_TRUE(manager.allTrades().empty());
  EXPECT_EQ(manager.statistics("AAA")->volume, 10u);
  EXPECT_FALSE(manager.storesHistory());
}

TEST(TradeManager, LedgerReferencesAreStable) {
  TradeManager manager;
  TradeManager::SymbolLedger& first = manager.ledger("AAA");
  for (int i = 0; i < 100; ++i) {
    manager.ledger("SYM" + std::to_string(i));  // Force rehashing.
  }
  EXPECT_EQ(&first, &manager.ledger("AAA"));
  first.record(makeTrade(1, "AAA", 100, 1));
  EXPECT_EQ(manager.tradeCount(), 1u);
  EXPECT_EQ(manager.allStatistics().size(), 101u);
}

TEST(TradeManager, NotifiesSubscribedListeners) {
  TradeManager manager;
  std::vector<TradeId> seen;
  const auto id = manager.subscribe([&seen](const Trade& t) { seen.push_back(t.id); });
  EXPECT_EQ(manager.listenerCount(), 1u);

  manager.notify({makeTrade(1, "AAA", 1, 1), makeTrade(2, "AAA", 1, 1)});
  EXPECT_EQ(seen, (std::vector<TradeId>{1, 2}));

  EXPECT_TRUE(manager.unsubscribe(id));
  EXPECT_FALSE(manager.unsubscribe(id));
  manager.notify({makeTrade(3, "AAA", 1, 1)});
  EXPECT_EQ(seen.size(), 2u);
}

TEST(TradeManager, ThrowingListenerDoesNotBreakOthers) {
  TradeManager manager;
  int calls = 0;
  manager.subscribe([](const Trade&) { throw std::runtime_error("bad listener"); });
  manager.subscribe([&calls](const Trade&) { ++calls; });
  EXPECT_NO_THROW(manager.notify({makeTrade(1, "AAA", 1, 1)}));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(manager.listenerErrors(), 1u);
}
