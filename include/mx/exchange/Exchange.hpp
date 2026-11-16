#pragma once

/// @file Exchange.hpp
/// Exchange management module: the thread-safe facade clients talk to.
///
/// It owns one MatchingEngine per symbol, assigns order ids, validates
/// requests, journals accepted commands, records trades and collects metrics.
///
/// Concurrency model
///   * Symbol registry: a fixed-capacity open-addressing hash table of atomic
///     pointers. Lookups (every order) are lock-free: no shared counter is
///     written, so threads trading different symbols never contend on it.
///     addSymbol is serialised by a writer mutex. Symbols are never removed,
///     so a published book pointer stays valid forever.
///   * Per-symbol lock (adaptive SpinMutex): orders on different symbols run in parallel;
///     orders on the same symbol are serialised, which is exactly what
///     price-time priority requires.
///   * Ids: lock-free atomics. Metrics: per-thread striped atomics.
///   * Lock order is always  registry writer -> symbol book -> {ledger, journal};
///     no code path acquires them in the opposite order, so no deadlock.
///   * Trade listeners run after the book lock is released, so a listener may
///     safely call back into the exchange.

#include "mx/core/ExecutionReport.hpp"
#include "mx/core/IdGenerator.hpp"
#include "mx/core/Order.hpp"
#include "mx/matching/MatchingEngine.hpp"
#include "mx/metrics/PerformanceMonitor.hpp"
#include "mx/persistence/EventJournal.hpp"
#include "mx/trade/TradeManager.hpp"
#include "mx/util/SpinMutex.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mx {

struct ExchangeConfig {
  bool storeTradeHistory{true};  ///< Keep every trade in memory (statistics are always kept).
  bool enableMetrics{true};      ///< Measure per-call latency and throughput.
  std::string journalPath;       ///< Write-ahead journal file; empty disables journaling.
  bool flushJournalOnWrite{false};
  std::size_t maxSymbols{1024};  ///< Capacity of the lock-free symbol table.
};

/// Top of book.
struct Quote {
  std::string symbol;
  std::optional<Price> bidPrice;
  Quantity bidQuantity{0};
  std::optional<Price> askPrice;
  Quantity askQuantity{0};
};

struct RecoveryStats {
  std::size_t symbolsAdded{0};
  std::size_t ordersReplayed{0};
  std::size_t cancelsReplayed{0};
  std::size_t tradesReplayed{0};
  std::size_t eventsSkipped{0};
  std::size_t malformedLines{0};
};

class Exchange {
 public:
  explicit Exchange(ExchangeConfig config = {});
  ~Exchange();

  Exchange(const Exchange&) = delete;
  Exchange& operator=(const Exchange&) = delete;

  // ---- Symbols -------------------------------------------------------------
  /// Lists a new symbol. Returns false if the symbol is invalid, already
  /// listed, or the symbol table is full (ExchangeConfig::maxSymbols).
  bool addSymbol(const std::string& symbol);
  [[nodiscard]] bool hasSymbol(const std::string& symbol) const;
  [[nodiscard]] std::vector<std::string> symbols() const;  ///< Sorted.

  // ---- Orders --------------------------------------------------------------
  ExecutionReport submitOrder(const OrderRequest& request);
  CancelResult cancelOrder(const std::string& symbol, OrderId orderId);
  /// Cancels by id alone by searching every book: O(number of symbols).
  CancelResult cancelOrder(OrderId orderId);

  // ---- Queries (consistent snapshots, safe to call concurrently) -----------
  [[nodiscard]] std::optional<Order> findOrder(const std::string& symbol, OrderId orderId) const;
  [[nodiscard]] std::optional<BookSnapshot> snapshot(
      const std::string& symbol, std::size_t depth = OrderBook::kAllLevels) const;
  [[nodiscard]] std::vector<Order> restingOrders(const std::string& symbol, Side side) const;
  [[nodiscard]] std::optional<Quote> quote(const std::string& symbol) const;
  [[nodiscard]] std::size_t restingOrderCount() const;

  // ---- Persistence ---------------------------------------------------------
  /// Replays a journal into this exchange (call before accepting new orders).
  /// Replayed commands are not re-journaled and do not notify listeners.
  RecoveryStats recover(const std::string& journalPath);
  void flushJournal();

  // ---- Components ----------------------------------------------------------
  [[nodiscard]] TradeManager& trades() noexcept { return tradeManager_; }
  [[nodiscard]] const TradeManager& trades() const noexcept { return tradeManager_; }
  [[nodiscard]] PerformanceMonitor& metrics() noexcept { return metrics_; }
  [[nodiscard]] const PerformanceMonitor& metrics() const noexcept { return metrics_; }
  [[nodiscard]] const ExchangeConfig& config() const noexcept { return config_; }

 private:
  /// Book critical sections are sub-microsecond, so a spin lock hands over
  /// far faster than a sleeping mutex under contention (see SpinMutex.hpp).
  using BookMutex = SpinMutex;
  using BookLock = std::lock_guard<BookMutex>;

  struct SymbolBook {
    SymbolBook(const std::string& symbol, IdGenerator& tradeIds,
               TradeManager::SymbolLedger& symbolLedger)
        : engine(symbol, tradeIds), ledger(&symbolLedger) {}

    mutable BookMutex mutex;
    MatchingEngine engine;
    TradeManager::SymbolLedger* ledger;
  };

  bool addSymbolInternal(const std::string& symbol, bool journal);
  [[nodiscard]] SymbolBook* findBook(const std::string& symbol) const;
  [[nodiscard]] std::vector<SymbolBook*> allBooks() const;
  ExecutionReport execute(SymbolBook& book, Order order, bool journal, bool notify);
  CancelResult cancelInBook(SymbolBook& book, OrderId orderId, bool journal);

  ExchangeConfig config_;
  IdGenerator orderIds_{1};
  IdGenerator tradeIds_{1};
  TradeManager tradeManager_;
  PerformanceMonitor metrics_;
  std::unique_ptr<EventJournal> journal_;

  // Symbol registry. ownedBooks_ is only touched under registryWriteMutex_;
  // slots_ is read lock-free (acquire) and written under the mutex (release).
  mutable std::mutex registryWriteMutex_;
  std::vector<std::unique_ptr<SymbolBook>> ownedBooks_;
  std::unique_ptr<std::atomic<SymbolBook*>[]> slots_;
  std::size_t slotMask_{0};
};

}  // namespace mx
