#pragma once

/// @file EventJournal.hpp
/// Write-ahead journal of accepted commands, used for crash recovery.
///
/// Every accepted command is appended *before* it is applied, while the
/// symbol's lock is held, so the journal order per symbol is exactly the order
/// in which the engine saw the commands. Replaying the journal through the
/// deterministic matching engine rebuilds identical order books and trades
/// (event sourcing).
///
/// Format: one CSV line per event, '#' starts a comment.
///   S,<symbol>
///   N,<orderId>,<timestamp>,<symbol>,<side>,<type>,<tif>,<priceTicks>,<qty>,<trader>
///   C,<orderId>,<timestamp>,<symbol>

#include "mx/core/Order.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mx {

enum class JournalEventType : std::uint8_t { AddSymbol, NewOrder, CancelOrder };

struct JournalEvent {
  JournalEventType type{JournalEventType::NewOrder};
  std::string symbol;
  OrderId orderId{kInvalidOrderId};
  Timestamp timestamp{0};
  Order order;  ///< Populated for NewOrder events.
};

struct JournalReadResult {
  std::vector<JournalEvent> events;
  std::size_t malformedLines{0};
};

class EventJournal {
 public:
  /// Opens (or creates) the journal for appending.
  /// @throws std::runtime_error if the file cannot be opened.
  explicit EventJournal(std::string path, bool flushOnWrite = false);
  ~EventJournal();

  EventJournal(const EventJournal&) = delete;
  EventJournal& operator=(const EventJournal&) = delete;

  void appendAddSymbol(const std::string& symbol);
  void appendNewOrder(const Order& order);
  void appendCancel(const std::string& symbol, OrderId orderId, Timestamp timestamp);
  void flush();

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::uint64_t eventsWritten() const noexcept {
    return eventsWritten_.load(std::memory_order_relaxed);
  }

  /// Reads all events. Malformed lines (for example a torn final write after a
  /// crash) are skipped and counted.
  /// @throws std::runtime_error if the file cannot be opened.
  [[nodiscard]] static JournalReadResult read(const std::string& path);

  [[nodiscard]] static std::string encodeAddSymbol(const std::string& symbol);
  [[nodiscard]] static std::string encodeNewOrder(const Order& order);
  [[nodiscard]] static std::string encodeCancel(const std::string& symbol, OrderId orderId,
                                                Timestamp timestamp);
  [[nodiscard]] static std::optional<JournalEvent> decode(std::string_view line);

 private:
  void writeLine(const std::string& line);

  std::string path_;
  bool flushOnWrite_;
  std::mutex mutex_;
  std::ofstream out_;
  std::atomic<std::uint64_t> eventsWritten_{0};
};

}  // namespace mx
