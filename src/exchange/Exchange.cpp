#include "mx/exchange/Exchange.hpp"

#include "mx/util/Clock.hpp"

#include <algorithm>
#include <utility>

namespace mx {
namespace {

Quantity tradedVolume(const std::vector<Trade>& trades) noexcept {
  Quantity volume = 0;
  for (const Trade& trade : trades) {
    volume += trade.quantity;
  }
  return volume;
}

/// Table size: a power of two at least twice the symbol capacity, so the load
/// factor never exceeds 0.5 and linear probing always finds an empty slot.
std::size_t slotCountFor(std::size_t maxSymbols) noexcept {
  std::size_t slots = 16;
  while (slots < maxSymbols * 2) {
    slots <<= 1;
  }
  return slots;
}

}  // namespace

Exchange::Exchange(ExchangeConfig config)
    : config_(std::move(config)), tradeManager_(config_.storeTradeHistory) {
  if (config_.maxSymbols == 0) {
    config_.maxSymbols = 1;
  }
  const std::size_t slotCount = slotCountFor(config_.maxSymbols);
  slots_ = std::make_unique<std::atomic<SymbolBook*>[]>(slotCount);  // value-initialised: null
  slotMask_ = slotCount - 1;
  ownedBooks_.reserve(config_.maxSymbols);  // push_back in addSymbol can then never throw.
  if (!config_.journalPath.empty()) {
    journal_ = std::make_unique<EventJournal>(config_.journalPath, config_.flushJournalOnWrite);
  }
}

Exchange::~Exchange() = default;

// ---- Symbols ----------------------------------------------------------------

bool Exchange::addSymbol(const std::string& symbol) { return addSymbolInternal(symbol, true); }

bool Exchange::addSymbolInternal(const std::string& symbol, bool journal) {
  if (!isValidSymbol(symbol)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(registryWriteMutex_);
  if (findBook(symbol) != nullptr || ownedBooks_.size() >= config_.maxSymbols) {
    return false;
  }
  TradeManager::SymbolLedger& ledger = tradeManager_.ledger(symbol);
  auto book = std::make_unique<SymbolBook>(symbol, tradeIds_, ledger);
  SymbolBook* published = book.get();
  if (journal && journal_) {
    journal_->appendAddSymbol(symbol);
  }
  ownedBooks_.push_back(std::move(book));

  const std::size_t hash = std::hash<std::string>{}(symbol);
  for (std::size_t probe = 0;; ++probe) {
    std::atomic<SymbolBook*>& slot = slots_[(hash + probe) & slotMask_];
    if (slot.load(std::memory_order_relaxed) == nullptr) {
      // Release: a reader that sees the pointer also sees the fully built book.
      slot.store(published, std::memory_order_release);
      return true;
    }
  }
}

bool Exchange::hasSymbol(const std::string& symbol) const { return findBook(symbol) != nullptr; }

std::vector<std::string> Exchange::symbols() const {
  std::vector<std::string> out;
  for (const SymbolBook* book : allBooks()) {
    out.push_back(book->engine.symbol());
  }
  std::sort(out.begin(), out.end());
  return out;
}

Exchange::SymbolBook* Exchange::findBook(const std::string& symbol) const {
  // Lock-free lookup. Entries are never removed, so an empty slot ends the probe.
  const std::size_t hash = std::hash<std::string>{}(symbol);
  for (std::size_t probe = 0; probe <= slotMask_; ++probe) {
    SymbolBook* book = slots_[(hash + probe) & slotMask_].load(std::memory_order_acquire);
    if (book == nullptr) {
      return nullptr;
    }
    if (book->engine.symbol() == symbol) {
      return book;
    }
  }
  return nullptr;
}

std::vector<Exchange::SymbolBook*> Exchange::allBooks() const {
  std::lock_guard<std::mutex> lock(registryWriteMutex_);
  std::vector<SymbolBook*> out;
  out.reserve(ownedBooks_.size());
  for (const auto& book : ownedBooks_) {
    out.push_back(book.get());
  }
  return out;
}

// ---- Orders -----------------------------------------------------------------

ExecutionReport Exchange::submitOrder(const OrderRequest& request) {
  const std::uint64_t start = config_.enableMetrics ? steadyNanos() : 0;
  const OrderId id = orderIds_.next();

  ExecutionReport report;
  if (auto error = validationError(request)) {
    report = ExecutionReport::rejected(id, request, std::move(*error));
  } else if (SymbolBook* book = findBook(request.symbol); book == nullptr) {
    report = ExecutionReport::rejected(id, request, "unknown symbol '" + request.symbol + "'");
  } else {
    report = execute(*book, Order(id, request, nowNanos()), /*journal=*/true, /*notify=*/true);
  }

  if (config_.enableMetrics) {
    metrics_.recordOrder(steadyNanos() - start, report.accepted(), report.trades.size(),
                         tradedVolume(report.trades));
  }
  return report;
}

ExecutionReport Exchange::execute(SymbolBook& book, Order order, bool journal, bool notify) {
  ExecutionReport report;
  {
    BookLock lock(book.mutex);
    if (journal && journal_) {
      journal_->appendNewOrder(order);  // Write-ahead: log before applying.
    }
    report = book.engine.process(std::move(order));
    book.ledger->record(report.trades);  // Inside the lock: ledger order == match order.
  }
  if (notify) {
    tradeManager_.notify(report.trades);  // Outside the lock: listeners may re-enter.
  }
  return report;
}

CancelResult Exchange::cancelInBook(SymbolBook& book, OrderId orderId, bool journal) {
  BookLock lock(book.mutex);
  if (journal && journal_ && book.engine.book().contains(orderId)) {
    journal_->appendCancel(book.engine.symbol(), orderId, nowNanos());
  }
  return book.engine.cancel(orderId);
}

CancelResult Exchange::cancelOrder(const std::string& symbol, OrderId orderId) {
  const std::uint64_t start = config_.enableMetrics ? steadyNanos() : 0;
  CancelResult result;
  if (SymbolBook* book = findBook(symbol); book != nullptr) {
    result = cancelInBook(*book, orderId, /*journal=*/true);
  } else {
    result.orderId = orderId;
    result.symbol = symbol;
    result.message = "unknown symbol '" + symbol + "'";
  }
  if (config_.enableMetrics) {
    metrics_.recordCancel(steadyNanos() - start, result.success);
  }
  return result;
}

CancelResult Exchange::cancelOrder(OrderId orderId) {
  const std::uint64_t start = config_.enableMetrics ? steadyNanos() : 0;
  CancelResult result;
  result.orderId = orderId;
  result.message = "order not found (unknown, already filled or already cancelled)";
  for (SymbolBook* book : allBooks()) {
    // Ids are unique across symbols, so at most one book can hold the order.
    CancelResult attempt = cancelInBook(*book, orderId, /*journal=*/true);
    if (attempt.success) {
      result = std::move(attempt);
      break;
    }
  }
  if (config_.enableMetrics) {
    metrics_.recordCancel(steadyNanos() - start, result.success);
  }
  return result;
}

// ---- Queries ----------------------------------------------------------------

std::optional<Order> Exchange::findOrder(const std::string& symbol, OrderId orderId) const {
  const SymbolBook* book = findBook(symbol);
  if (book == nullptr) {
    return std::nullopt;
  }
  BookLock lock(book->mutex);
  return book->engine.book().find(orderId);
}

std::optional<BookSnapshot> Exchange::snapshot(const std::string& symbol,
                                               std::size_t depth) const {
  const SymbolBook* book = findBook(symbol);
  if (book == nullptr) {
    return std::nullopt;
  }
  BookLock lock(book->mutex);
  return book->engine.book().snapshot(depth);
}

std::vector<Order> Exchange::restingOrders(const std::string& symbol, Side side) const {
  const SymbolBook* book = findBook(symbol);
  if (book == nullptr) {
    return {};
  }
  BookLock lock(book->mutex);
  return book->engine.book().orders(side);
}

std::optional<Quote> Exchange::quote(const std::string& symbol) const {
  const SymbolBook* book = findBook(symbol);
  if (book == nullptr) {
    return std::nullopt;
  }
  BookLock lock(book->mutex);
  const OrderBook& orderBook = book->engine.book();
  Quote q;
  q.symbol = symbol;
  q.bidPrice = orderBook.bestBid();
  q.askPrice = orderBook.bestAsk();
  if (q.bidPrice) {
    q.bidQuantity = orderBook.volumeAt(Side::Buy, *q.bidPrice);
  }
  if (q.askPrice) {
    q.askQuantity = orderBook.volumeAt(Side::Sell, *q.askPrice);
  }
  return q;
}

std::size_t Exchange::restingOrderCount() const {
  std::size_t total = 0;
  for (const SymbolBook* book : allBooks()) {
    BookLock lock(book->mutex);
    total += book->engine.book().orderCount();
  }
  return total;
}

// ---- Persistence ------------------------------------------------------------

RecoveryStats Exchange::recover(const std::string& journalPath) {
  JournalReadResult journal = EventJournal::read(journalPath);
  RecoveryStats stats;
  stats.malformedLines = journal.malformedLines;

  for (JournalEvent& event : journal.events) {
    switch (event.type) {
      case JournalEventType::AddSymbol:
        if (addSymbolInternal(event.symbol, /*journal=*/false)) {
          ++stats.symbolsAdded;
        }
        break;
      case JournalEventType::NewOrder: {
        orderIds_.ensureAbove(event.orderId);
        SymbolBook* book = findBook(event.symbol);
        if (book == nullptr) {
          ++stats.eventsSkipped;
          break;
        }
        const ExecutionReport report =
            execute(*book, std::move(event.order), /*journal=*/false, /*notify=*/false);
        stats.tradesReplayed += report.trades.size();
        ++stats.ordersReplayed;
        break;
      }
      case JournalEventType::CancelOrder: {
        SymbolBook* book = findBook(event.symbol);
        if (book == nullptr) {
          ++stats.eventsSkipped;
          break;
        }
        if (cancelInBook(*book, event.orderId, /*journal=*/false).success) {
          ++stats.cancelsReplayed;
        } else {
          ++stats.eventsSkipped;
        }
        break;
      }
    }
  }
  return stats;
}

void Exchange::flushJournal() {
  if (journal_) {
    journal_->flush();
  }
}

}  // namespace mx
