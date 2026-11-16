#include "mx/persistence/EventJournal.hpp"

#include "mx/util/StringUtils.hpp"

#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mx {

EventJournal::EventJournal(std::string path, bool flushOnWrite)
    : path_(std::move(path)), flushOnWrite_(flushOnWrite) {
  std::error_code ec;
  const bool fresh =
      !std::filesystem::exists(path_, ec) || std::filesystem::file_size(path_, ec) == 0;
  out_.open(path_, std::ios::out | std::ios::app);
  if (!out_.is_open()) {
    throw std::runtime_error("cannot open journal '" + path_ + "' for writing");
  }
  if (fresh) {
    out_ << "# MiniExchange journal v1\n";
    out_.flush();
  }
}

EventJournal::~EventJournal() {
  std::lock_guard<std::mutex> lock(mutex_);
  out_.flush();
}

void EventJournal::appendAddSymbol(const std::string& symbol) {
  writeLine(encodeAddSymbol(symbol));
}

void EventJournal::appendNewOrder(const Order& order) { writeLine(encodeNewOrder(order)); }

void EventJournal::appendCancel(const std::string& symbol, OrderId orderId, Timestamp timestamp) {
  writeLine(encodeCancel(symbol, orderId, timestamp));
}

void EventJournal::flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  out_.flush();
}

void EventJournal::writeLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(mutex_);
  out_ << line << '\n';
  if (flushOnWrite_) {
    out_.flush();
  }
  if (!out_) {
    throw std::runtime_error("failed to write to journal '" + path_ + "'");
  }
  eventsWritten_.fetch_add(1, std::memory_order_relaxed);
}

std::string EventJournal::encodeAddSymbol(const std::string& symbol) { return "S," + symbol; }

std::string EventJournal::encodeNewOrder(const Order& order) {
  std::string line = "N,";
  line += std::to_string(order.id());
  line += ',';
  line += std::to_string(order.timestamp());
  line += ',';
  line += order.symbol();
  line += ',';
  line += toString(order.side());
  line += ',';
  line += toString(order.type());
  line += ',';
  line += toString(order.timeInForce());
  line += ',';
  line += std::to_string(order.price());
  line += ',';
  line += std::to_string(order.quantity());
  line += ',';
  line += order.trader();
  return line;
}

std::string EventJournal::encodeCancel(const std::string& symbol, OrderId orderId,
                                       Timestamp timestamp) {
  return "C," + std::to_string(orderId) + ',' + std::to_string(timestamp) + ',' + symbol;
}

std::optional<JournalEvent> EventJournal::decode(std::string_view line) {
  const auto fields = split(line, ',');
  if (fields.empty()) {
    return std::nullopt;
  }
  JournalEvent event;

  if (fields[0] == "S" && fields.size() == 2) {
    if (!isValidSymbol(fields[1])) {
      return std::nullopt;
    }
    event.type = JournalEventType::AddSymbol;
    event.symbol = std::string(fields[1]);
    return event;
  }

  if (fields[0] == "N" && fields.size() == 10) {
    const auto id = parseInteger<OrderId>(fields[1]);
    const auto timestamp = parseInteger<Timestamp>(fields[2]);
    const auto side = parseSide(fields[4]);
    const auto type = parseOrderType(fields[5]);
    const auto tif = parseTimeInForce(fields[6]);
    const auto price = parseInteger<Price>(fields[7]);
    const auto quantity = parseInteger<Quantity>(fields[8]);
    if (!id || *id == kInvalidOrderId || !timestamp || !isValidSymbol(fields[3]) || !side ||
        !type || !tif || !price || !quantity || *quantity == 0 || !isValidTrader(fields[9])) {
      return std::nullopt;
    }
    event.type = JournalEventType::NewOrder;
    event.symbol = std::string(fields[3]);
    event.orderId = *id;
    event.timestamp = *timestamp;
    event.order = Order(*id, event.symbol, *side, *type, *tif, *price, *quantity,
                        std::string(fields[9]), *timestamp);
    return event;
  }

  if (fields[0] == "C" && fields.size() == 4) {
    const auto id = parseInteger<OrderId>(fields[1]);
    const auto timestamp = parseInteger<Timestamp>(fields[2]);
    if (!id || !timestamp || !isValidSymbol(fields[3])) {
      return std::nullopt;
    }
    event.type = JournalEventType::CancelOrder;
    event.orderId = *id;
    event.timestamp = *timestamp;
    event.symbol = std::string(fields[3]);
    return event;
  }

  return std::nullopt;
}

JournalReadResult EventJournal::read(const std::string& path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    throw std::runtime_error("cannot open journal '" + path + "' for reading");
  }
  JournalReadResult result;
  std::string line;
  while (std::getline(in, line)) {
    const std::string_view view = trim(line);
    if (view.empty() || view.front() == '#') {
      continue;
    }
    if (auto event = decode(view)) {
      result.events.push_back(std::move(*event));
    } else {
      ++result.malformedLines;
    }
  }
  return result;
}

}  // namespace mx
