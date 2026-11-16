// Multithreading tests. Run under ThreadSanitizer in CI (MX_ENABLE_TSAN=ON).

#include "mx/exchange/Exchange.hpp"
#include "mx/exchange/OrderGateway.hpp"
#include "mx/util/ThreadSafeQueue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace mx;

TEST(ThreadSafeQueue, ProducersAndConsumersExchangeEveryItem) {
  ThreadSafeQueue<int> queue;
  constexpr int kProducers = 4;
  constexpr int kItemsEach = 10'000;
  std::atomic<long long> sum{0};
  std::atomic<int> received{0};

  std::vector<std::thread> consumers;
  for (int c = 0; c < 3; ++c) {
    consumers.emplace_back([&] {
      while (auto item = queue.waitPop()) {
        sum.fetch_add(*item);
        received.fetch_add(1);
      }
    });
  }
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&queue] {
      for (int i = 1; i <= kItemsEach; ++i) {
        queue.push(i);
      }
    });
  }
  for (auto& t : producers) t.join();
  queue.close();
  for (auto& t : consumers) t.join();

  EXPECT_EQ(received.load(), kProducers * kItemsEach);
  EXPECT_EQ(sum.load(), static_cast<long long>(kProducers) * kItemsEach * (kItemsEach + 1) / 2);
  EXPECT_FALSE(queue.push(1));  // Closed.
  EXPECT_FALSE(queue.tryPop().has_value());
}

TEST(ExchangeConcurrency, ParallelTradingPreservesQuantityConservation) {
  Exchange exchange;
  const std::vector<std::string> symbols = {"S0", "S1", "S2", "S3"};
  for (const auto& s : symbols) exchange.addSymbol(s);

  struct OrderRecord {
    std::string symbol;
    Quantity quantity{0};
    Quantity cancelled{0};
  };
  std::mutex recordsMutex;
  std::map<OrderId, OrderRecord> records;
  std::vector<Trade> allTrades;

  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 3000;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t * 7919 + 1));
      std::vector<std::pair<std::string, OrderId>> mine;
      std::map<OrderId, OrderRecord> localRecords;
      std::vector<Trade> localTrades;
      std::vector<std::pair<OrderId, Quantity>> localCancels;
      for (int i = 0; i < kOpsPerThread; ++i) {
        const std::string& symbol = symbols[rng() % symbols.size()];
        if (rng() % 5 == 0 && !mine.empty()) {
          const auto& [sym, id] = mine[rng() % mine.size()];
          const CancelResult result = exchange.cancelOrder(sym, id);
          if (result.success) localCancels.emplace_back(id, result.cancelledQuantity);
          continue;
        }
        const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
        const Quantity qty = 1 + rng() % 30;
        const ExecutionReport report =
            rng() % 8 == 0
                ? exchange.submitOrder(OrderRequest::market(symbol, side, qty))
                : exchange.submitOrder(
                      OrderRequest::limit(symbol, side, qty, 100 + static_cast<Price>(rng() % 10)));
        ASSERT_TRUE(report.accepted());
        ASSERT_EQ(report.quantity,
                  report.filledQuantity + report.restingQuantity + report.cancelledQuantity);
        localRecords[report.orderId] = OrderRecord{symbol, qty, report.cancelledQuantity};
        localTrades.insert(localTrades.end(), report.trades.begin(), report.trades.end());
        mine.emplace_back(symbol, report.orderId);
      }
      std::lock_guard<std::mutex> lock(recordsMutex);
      for (auto& [id, record] : localRecords) records[id] = record;
      for (const auto& [id, qty] : localCancels) records[id].cancelled += qty;
      allTrades.insert(allTrades.end(), localTrades.begin(), localTrades.end());
    });
  }
  for (auto& t : threads) t.join();

  // 1. Every trade is unique and recorded once in the ledger.
  std::set<TradeId> tradeIds;
  std::map<OrderId, Quantity> traded;
  Quantity volume = 0;
  for (const Trade& trade : allTrades) {
    EXPECT_TRUE(tradeIds.insert(trade.id).second) << "duplicate trade id " << trade.id;
    traded[trade.buyOrderId] += trade.quantity;
    traded[trade.sellOrderId] += trade.quantity;
    volume += trade.quantity;
  }
  EXPECT_EQ(exchange.trades().tradeCount(), allTrades.size());
  EXPECT_EQ(exchange.trades().totalVolume(), volume);

  // 2. No order traded more than it had, and every unit is accounted for:
  //    original = traded + cancelled + still resting.
  for (const auto& [id, record] : records) {
    const auto resting = exchange.findOrder(record.symbol, id);
    const Quantity restingQty = resting ? resting->remainingQuantity() : 0;
    ASSERT_EQ(record.quantity, traded[id] + record.cancelled + restingQty) << "order " << id;
  }

  // 3. No book is left crossed.
  for (const auto& s : symbols) {
    const auto q = exchange.quote(s);
    if (q->bidPrice && q->askPrice) {
      EXPECT_LT(*q->bidPrice, *q->askPrice) << s;
    }
  }
}

TEST(ExchangeConcurrency, ReadersRunAlongsideWriters) {
  Exchange exchange;
  exchange.addSymbol("RW");
  std::atomic<bool> done{false};
  std::atomic<long> reads{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&] {
      while (!done.load()) {
        const auto snap = exchange.snapshot("RW", 5);
        const auto q = exchange.quote("RW");
        (void)exchange.trades().recentTrades(5);
        (void)exchange.metrics().snapshot();
        ASSERT_TRUE(snap.has_value());
        if (q->bidPrice && q->askPrice) {
          ASSERT_LT(*q->bidPrice, *q->askPrice);
        }
        reads.fetch_add(1);
      }
    });
  }
  std::vector<std::thread> writers;
  for (int w = 0; w < 2; ++w) {
    writers.emplace_back([&, w] {
      std::mt19937 rng(static_cast<unsigned>(w + 11));
      for (int i = 0; i < 5000; ++i) {
        const Side side = rng() % 2 == 0 ? Side::Buy : Side::Sell;
        exchange.submitOrder(
            OrderRequest::limit("RW", side, 1 + rng() % 10, 50 + static_cast<Price>(rng() % 5)));
      }
    });
  }
  for (auto& t : writers) t.join();
  done.store(true);
  for (auto& t : readers) t.join();
  EXPECT_GT(reads.load(), 0);
  EXPECT_EQ(exchange.metrics().snapshot().ordersReceived, 10'000u);
}

TEST(OrderGateway, ProcessesEveryRequestAndPreservesPerSymbolOrder) {
  Exchange exchange;
  const std::vector<std::string> symbols = {"G0", "G1", "G2", "G3", "G4", "G5"};
  for (const auto& s : symbols) exchange.addSymbol(s);

  std::vector<std::pair<std::string, std::future<ExecutionReport>>> futures;
  {
    OrderGateway gateway(exchange, 4);
    EXPECT_EQ(gateway.workerCount(), 4u);
    for (int i = 0; i < 6000; ++i) {
      const std::string& symbol = symbols[static_cast<std::size_t>(i) % symbols.size()];
      const Side side = i % 2 == 0 ? Side::Buy : Side::Sell;
      futures.emplace_back(symbol, gateway.submit(OrderRequest::limit(
                                       symbol, side, 10, 100 + static_cast<Price>(i % 3))));
    }
  }  // Destructor drains the queues and joins the workers.

  // Requests for one symbol run on one worker in submission order, so the ids
  // the exchange assigns must increase in submission order per symbol.
  std::map<std::string, OrderId> lastId;
  for (auto& [symbol, future] : futures) {
    ASSERT_EQ(future.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    const ExecutionReport report = future.get();
    EXPECT_TRUE(report.accepted());
    EXPECT_GT(report.orderId, lastId[symbol]) << symbol;
    lastId[symbol] = report.orderId;
  }
  EXPECT_EQ(exchange.metrics().snapshot().ordersReceived, 6000u);
}

TEST(OrderGateway, CancelsAndShutdownSemantics) {
  Exchange exchange;
  exchange.addSymbol("GC");
  OrderGateway gateway(exchange, 2);
  const ExecutionReport placed =
      gateway.submit(OrderRequest::limit("GC", Side::Buy, 5, 100)).get();
  const CancelResult cancelled = gateway.cancel("GC", placed.orderId).get();
  EXPECT_TRUE(cancelled.success);

  gateway.shutdown();
  gateway.shutdown();  // Idempotent.
  EXPECT_THROW((void)gateway.submit(OrderRequest::limit("GC", Side::Buy, 1, 1)),
               std::runtime_error);
  EXPECT_EQ(gateway.tasksProcessed(), 2u);
}
