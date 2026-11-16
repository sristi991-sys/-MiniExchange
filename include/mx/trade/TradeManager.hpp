#pragma once

/// @file TradeManager.hpp
/// Trade management module: the transaction record (trade ledger), per-symbol
/// market statistics and trade listeners (observer pattern).
///
/// Concurrency: the ledger is sharded per symbol (lock striping). Each
/// SymbolLedger has its own mutex, so trades on different symbols are recorded
/// in parallel without contending on one global lock. Listeners are stored as
/// an immutable copy-on-write list, so notification never holds a lock while
/// running user callbacks.

#include "mx/core/Trade.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mx {

struct SymbolStatistics {
  std::string symbol;
  std::uint64_t tradeCount{0};
  Quantity volume{0};
  double notional{0.0};  ///< Sum of price * quantity, in ticks.
  Price openPrice{0};
  Price lastPrice{0};
  Price highPrice{0};
  Price lowPrice{0};

  /// Volume-weighted average price in currency units (e.g. 150.25); 0 with no trades.
  [[nodiscard]] double vwap() const noexcept;
};

class TradeManager {
 public:
  using TradeListener = std::function<void(const Trade&)>;
  using ListenerId = std::uint64_t;

  /// Trade history and statistics for one symbol.
  class SymbolLedger {
   public:
    SymbolLedger(std::string symbol, bool storeHistory);
    SymbolLedger(const SymbolLedger&) = delete;
    SymbolLedger& operator=(const SymbolLedger&) = delete;

    void record(const Trade& trade);
    void record(const std::vector<Trade>& trades);

    [[nodiscard]] std::vector<Trade> trades() const;
    [[nodiscard]] SymbolStatistics statistics() const;
    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }

   private:
    void applyLocked(const Trade& trade);

    std::string symbol_;
    bool storeHistory_;
    mutable std::mutex mutex_;
    std::vector<Trade> trades_;
    SymbolStatistics stats_;
  };

  /// @param storeHistory keep every trade in memory. When false only the
  ///        statistics are maintained (useful for long benchmark runs).
  explicit TradeManager(bool storeHistory = true);

  TradeManager(const TradeManager&) = delete;
  TradeManager& operator=(const TradeManager&) = delete;

  /// Returns the ledger for `symbol`, creating it on first use. The reference
  /// stays valid for the lifetime of the TradeManager.
  SymbolLedger& ledger(const std::string& symbol);

  void record(const Trade& trade);
  void record(const std::vector<Trade>& trades);

  ListenerId subscribe(TradeListener listener);
  bool unsubscribe(ListenerId id);
  /// Invokes every listener for every trade. Exceptions thrown by a listener
  /// are caught and counted so a faulty observer cannot break the exchange.
  void notify(const std::vector<Trade>& trades) const;
  [[nodiscard]] std::size_t listenerCount() const;
  [[nodiscard]] std::uint64_t listenerErrors() const noexcept {
    return listenerErrors_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::vector<Trade> allTrades() const;  ///< Sorted by trade id.
  [[nodiscard]] std::vector<Trade> tradesForSymbol(const std::string& symbol) const;
  [[nodiscard]] std::vector<Trade> tradesForOrder(OrderId orderId) const;
  [[nodiscard]] std::vector<Trade> recentTrades(std::size_t count) const;

  [[nodiscard]] std::optional<SymbolStatistics> statistics(const std::string& symbol) const;
  [[nodiscard]] std::vector<SymbolStatistics> allStatistics() const;  ///< Sorted by symbol.
  [[nodiscard]] std::uint64_t tradeCount() const;
  [[nodiscard]] Quantity totalVolume() const;
  [[nodiscard]] bool storesHistory() const noexcept { return storeHistory_; }

 private:
  using ListenerList = std::vector<std::pair<ListenerId, TradeListener>>;

  [[nodiscard]] std::vector<const SymbolLedger*> ledgers() const;
  [[nodiscard]] const SymbolLedger* findLedger(const std::string& symbol) const;

  bool storeHistory_;

  mutable std::shared_mutex ledgerMutex_;
  std::unordered_map<std::string, std::unique_ptr<SymbolLedger>> ledgers_;

  mutable std::mutex listenerMutex_;
  std::shared_ptr<const ListenerList> listeners_;
  ListenerId nextListenerId_{1};
  std::atomic<bool> hasListeners_{false};
  mutable std::atomic<std::uint64_t> listenerErrors_{0};
};

}  // namespace mx
