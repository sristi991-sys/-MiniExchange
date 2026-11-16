#include "mx/trade/TradeManager.hpp"

#include <algorithm>

namespace mx {

double SymbolStatistics::vwap() const noexcept {
  if (volume == 0) {
    return 0.0;
  }
  return notional / static_cast<double>(volume) / static_cast<double>(kPriceScale);
}

// ---- SymbolLedger -----------------------------------------------------------

TradeManager::SymbolLedger::SymbolLedger(std::string symbol, bool storeHistory)
    : symbol_(std::move(symbol)), storeHistory_(storeHistory) {
  stats_.symbol = symbol_;
}

void TradeManager::SymbolLedger::record(const Trade& trade) {
  std::lock_guard<std::mutex> lock(mutex_);
  applyLocked(trade);
}

void TradeManager::SymbolLedger::record(const std::vector<Trade>& trades) {
  if (trades.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Trade& trade : trades) {
    applyLocked(trade);
  }
}

std::vector<Trade> TradeManager::SymbolLedger::trades() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return trades_;
}

SymbolStatistics TradeManager::SymbolLedger::statistics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void TradeManager::SymbolLedger::applyLocked(const Trade& trade) {
  if (storeHistory_) {
    trades_.push_back(trade);
  }
  if (stats_.tradeCount == 0) {
    stats_.openPrice = trade.price;
    stats_.highPrice = trade.price;
    stats_.lowPrice = trade.price;
  }
  ++stats_.tradeCount;
  stats_.volume += trade.quantity;
  stats_.notional += static_cast<double>(trade.price) * static_cast<double>(trade.quantity);
  stats_.lastPrice = trade.price;
  stats_.highPrice = std::max(stats_.highPrice, trade.price);
  stats_.lowPrice = std::min(stats_.lowPrice, trade.price);
}

// ---- TradeManager -----------------------------------------------------------

TradeManager::TradeManager(bool storeHistory)
    : storeHistory_(storeHistory), listeners_(std::make_shared<const ListenerList>()) {}

TradeManager::SymbolLedger& TradeManager::ledger(const std::string& symbol) {
  {
    std::shared_lock<std::shared_mutex> lock(ledgerMutex_);
    const auto it = ledgers_.find(symbol);
    if (it != ledgers_.end()) {
      return *it->second;
    }
  }
  std::unique_lock<std::shared_mutex> lock(ledgerMutex_);
  auto& slot = ledgers_[symbol];
  if (!slot) {
    slot = std::make_unique<SymbolLedger>(symbol, storeHistory_);
  }
  return *slot;
}

void TradeManager::record(const Trade& trade) { ledger(trade.symbol).record(trade); }

void TradeManager::record(const std::vector<Trade>& trades) {
  for (const Trade& trade : trades) {
    record(trade);
  }
}

TradeManager::ListenerId TradeManager::subscribe(TradeListener listener) {
  std::lock_guard<std::mutex> lock(listenerMutex_);
  auto updated = std::make_shared<ListenerList>(*listeners_);
  const ListenerId id = nextListenerId_++;
  updated->emplace_back(id, std::move(listener));
  listeners_ = std::move(updated);
  hasListeners_.store(true, std::memory_order_release);
  return id;
}

bool TradeManager::unsubscribe(ListenerId id) {
  std::lock_guard<std::mutex> lock(listenerMutex_);
  auto updated = std::make_shared<ListenerList>(*listeners_);
  const auto it = std::find_if(updated->begin(), updated->end(),
                               [id](const auto& entry) { return entry.first == id; });
  if (it == updated->end()) {
    return false;
  }
  updated->erase(it);
  hasListeners_.store(!updated->empty(), std::memory_order_release);
  listeners_ = std::move(updated);
  return true;
}

void TradeManager::notify(const std::vector<Trade>& trades) const {
  if (trades.empty() || !hasListeners_.load(std::memory_order_acquire)) {
    return;  // Fast path: no lock taken when nobody is listening.
  }
  std::shared_ptr<const ListenerList> snapshot;
  {
    std::lock_guard<std::mutex> lock(listenerMutex_);
    snapshot = listeners_;
  }
  for (const auto& entry : *snapshot) {
    for (const Trade& trade : trades) {
      try {
        entry.second(trade);
      } catch (...) {
        listenerErrors_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
}

std::size_t TradeManager::listenerCount() const {
  std::lock_guard<std::mutex> lock(listenerMutex_);
  return listeners_->size();
}

std::vector<const TradeManager::SymbolLedger*> TradeManager::ledgers() const {
  std::shared_lock<std::shared_mutex> lock(ledgerMutex_);
  std::vector<const SymbolLedger*> out;
  out.reserve(ledgers_.size());
  for (const auto& entry : ledgers_) {
    out.push_back(entry.second.get());
  }
  return out;
}

const TradeManager::SymbolLedger* TradeManager::findLedger(const std::string& symbol) const {
  std::shared_lock<std::shared_mutex> lock(ledgerMutex_);
  const auto it = ledgers_.find(symbol);
  return it == ledgers_.end() ? nullptr : it->second.get();
}

std::vector<Trade> TradeManager::allTrades() const {
  std::vector<Trade> out;
  for (const SymbolLedger* ledgerPtr : ledgers()) {
    auto trades = ledgerPtr->trades();
    out.insert(out.end(), std::make_move_iterator(trades.begin()),
               std::make_move_iterator(trades.end()));
  }
  std::sort(out.begin(), out.end(), [](const Trade& a, const Trade& b) { return a.id < b.id; });
  return out;
}

std::vector<Trade> TradeManager::tradesForSymbol(const std::string& symbol) const {
  const SymbolLedger* ledgerPtr = findLedger(symbol);
  return ledgerPtr == nullptr ? std::vector<Trade>{} : ledgerPtr->trades();
}

std::vector<Trade> TradeManager::tradesForOrder(OrderId orderId) const {
  std::vector<Trade> out;
  for (const Trade& trade : allTrades()) {
    if (trade.buyOrderId == orderId || trade.sellOrderId == orderId) {
      out.push_back(trade);
    }
  }
  return out;
}

std::vector<Trade> TradeManager::recentTrades(std::size_t count) const {
  std::vector<Trade> all = allTrades();
  if (all.size() > count) {
    all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(count));
  }
  return all;
}

std::optional<SymbolStatistics> TradeManager::statistics(const std::string& symbol) const {
  const SymbolLedger* ledgerPtr = findLedger(symbol);
  if (ledgerPtr == nullptr) {
    return std::nullopt;
  }
  return ledgerPtr->statistics();
}

std::vector<SymbolStatistics> TradeManager::allStatistics() const {
  std::vector<SymbolStatistics> out;
  for (const SymbolLedger* ledgerPtr : ledgers()) {
    out.push_back(ledgerPtr->statistics());
  }
  std::sort(out.begin(), out.end(),
            [](const SymbolStatistics& a, const SymbolStatistics& b) { return a.symbol < b.symbol; });
  return out;
}

std::uint64_t TradeManager::tradeCount() const {
  std::uint64_t total = 0;
  for (const SymbolLedger* ledgerPtr : ledgers()) {
    total += ledgerPtr->statistics().tradeCount;
  }
  return total;
}

Quantity TradeManager::totalVolume() const {
  Quantity total = 0;
  for (const SymbolLedger* ledgerPtr : ledgers()) {
    total += ledgerPtr->statistics().volume;
  }
  return total;
}

}  // namespace mx
