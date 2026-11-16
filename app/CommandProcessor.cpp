#include "CommandProcessor.hpp"

#include "mx/persistence/TradeStore.hpp"
#include "mx/util/StringUtils.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace mx::cli {
namespace {

constexpr std::size_t kDefaultDepth = 10;
constexpr std::size_t kDefaultTradeCount = 20;

/// HH:MM:SS.mmm (UTC) from nanoseconds since the epoch; no platform calls needed.
std::string timeOfDay(Timestamp nanos) {
  const auto millisTotal = nanos / 1'000'000;
  const auto millis = millisTotal % 1000;
  const auto secondsOfDay = (millisTotal / 1000) % 86'400;
  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << secondsOfDay / 3600 << ':' << std::setw(2)
      << (secondsOfDay / 60) % 60 << ':' << std::setw(2) << secondsOfDay % 60 << '.'
      << std::setw(3) << millis;
  return out.str();
}

std::string optionalPrice(const std::optional<Price>& price) {
  return price ? formatPrice(*price) : std::string("-");
}

std::string money(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(2) << value;
  return out.str();
}

}  // namespace

CommandProcessor::CommandProcessor(Exchange& exchange, std::ostream& out)
    : exchange_(exchange), out_(out) {}

std::string CommandProcessor::helpText() {
  return R"(Commands (case-insensitive; prices like 150 or 150.25):
  buy  <SYMBOL> <QTY> <PRICE|MKT> [GTC|IOC|FOK] [TRADER]   place a buy order
  sell <SYMBOL> <QTY> <PRICE|MKT> [GTC|IOC|FOK] [TRADER]   place a sell order
  cancel <ORDER_ID> [SYMBOL]          cancel a resting order
  book <SYMBOL> [DEPTH]               show the order book (default depth 10)
  orders <SYMBOL>                     list resting orders in priority order
  quote <SYMBOL>                      best bid / best ask
  trades [SYMBOL] [N]                 last N trades (default 20)
  stats                               per-symbol market statistics
  metrics                             engine performance metrics
  symbols                             list symbols
  addsymbol <SYMBOL>                  list a new symbol
  export <FILE.csv>                   write all trades to a CSV file
  help                                show this help
  quit                                exit
Examples:
  buy AAPL 100 150.25            limit buy, rests until filled or cancelled (GTC)
  sell AAPL 50 MKT               market sell
  buy AAPL 200 150 IOC alice     immediate-or-cancel, trader 'alice'
)";
}

bool CommandProcessor::execute(std::string_view rawLine) {
  const std::string_view line = trim(rawLine);
  if (line.empty() || line.front() == '#') {
    return true;
  }
  const std::vector<std::string> tokens = tokenize(line);
  const std::string command = toUpper(tokens.front());
  const Args args(tokens.begin() + 1, tokens.end());

  try {
    if (command == "BUY" || command == "B") {
      placeOrder(Side::Buy, args);
    } else if (command == "SELL" || command == "S") {
      placeOrder(Side::Sell, args);
    } else if (command == "CANCEL" || command == "C") {
      cancel(args);
    } else if (command == "BOOK") {
      showBook(args);
    } else if (command == "ORDERS") {
      showOrders(args);
    } else if (command == "QUOTE") {
      showQuote(args);
    } else if (command == "TRADES") {
      showTrades(args);
    } else if (command == "STATS") {
      showStats();
    } else if (command == "METRICS") {
      out_ << exchange_.metrics().report();
    } else if (command == "SYMBOLS") {
      showSymbols();
    } else if (command == "ADDSYMBOL") {
      addSymbol(args);
    } else if (command == "EXPORT") {
      exportTrades(args);
    } else if (command == "HELP" || command == "?") {
      out_ << helpText();
    } else if (command == "QUIT" || command == "EXIT" || command == "Q") {
      return false;
    } else {
      out_ << "Unknown command '" << tokens.front() << "'. Type 'help' for the command list.\n";
    }
  } catch (const std::exception& error) {
    out_ << "Error: " << error.what() << '\n';
  }
  return true;
}

void CommandProcessor::placeOrder(Side side, const Args& args) {
  if (args.size() < 3) {
    out_ << "Usage: " << (side == Side::Buy ? "buy" : "sell")
         << " <SYMBOL> <QTY> <PRICE|MKT> [GTC|IOC|FOK] [TRADER]\n";
    return;
  }
  OrderRequest request;
  request.side = side;
  request.symbol = toUpper(args[0]);

  const auto quantity = parseInteger<Quantity>(args[1]);
  if (!quantity || *quantity == 0) {
    out_ << "Error: quantity must be a positive whole number, got '" << args[1] << "'\n";
    return;
  }
  request.quantity = *quantity;

  const std::string priceToken = toUpper(args[2]);
  if (priceToken == "MKT" || priceToken == "MARKET") {
    request.type = OrderType::Market;
    request.timeInForce = TimeInForce::IOC;
  } else {
    const auto price = parsePrice(args[2]);
    if (!price || *price == 0) {
      out_ << "Error: invalid price '" << args[2] << "' (use e.g. 150 or 150.25, or MKT)\n";
      return;
    }
    request.type = OrderType::Limit;
    request.price = *price;
  }

  for (std::size_t i = 3; i < args.size(); ++i) {
    if (const auto tif = parseTimeInForce(args[i])) {
      request.timeInForce = *tif;
    } else if (request.trader.empty()) {
      request.trader = args[i];
    } else {
      out_ << "Error: unexpected argument '" << args[i] << "'\n";
      return;
    }
  }
  printReport(exchange_.submitOrder(request));
}

void CommandProcessor::printReport(const ExecutionReport& report) {
  out_ << "Order #" << report.orderId << ' ' << toString(report.side) << ' ' << report.quantity
       << ' ' << report.symbol << ' ';
  if (report.type == OrderType::Market) {
    out_ << "MKT";
  } else {
    out_ << '@' << formatPrice(report.price);
  }
  out_ << ' ' << toString(report.timeInForce) << " -> " << toString(report.status);

  if (!report.accepted()) {
    out_ << ": " << report.message << '\n';
    return;
  }
  if (report.filledQuantity > 0) {
    out_ << " | filled " << report.filledQuantity << " @ avg " << money(report.averageFillPrice());
  }
  if (report.restingQuantity > 0) {
    out_ << " | resting " << report.restingQuantity;
  }
  if (report.cancelledQuantity > 0) {
    out_ << " | cancelled " << report.cancelledQuantity;
  }
  out_ << '\n';
  for (const Trade& trade : report.trades) {
    out_ << "  " << describe(trade) << '\n';
  }
  if (!report.message.empty() && report.cancelledQuantity > 0) {
    out_ << "  note: " << report.message << '\n';
  }
}

void CommandProcessor::cancel(const Args& args) {
  if (args.empty()) {
    out_ << "Usage: cancel <ORDER_ID> [SYMBOL]\n";
    return;
  }
  const auto id = parseInteger<OrderId>(args[0]);
  if (!id) {
    out_ << "Error: invalid order id '" << args[0] << "'\n";
    return;
  }
  const CancelResult result =
      args.size() > 1 ? exchange_.cancelOrder(toUpper(args[1]), *id) : exchange_.cancelOrder(*id);
  if (result.success) {
    out_ << "Cancelled order #" << result.orderId << " (" << result.symbol << "), "
         << result.cancelledQuantity << " unfilled\n";
  } else {
    out_ << "Cancel failed for order #" << result.orderId << ": " << result.message << '\n';
  }
}

void CommandProcessor::showBook(const Args& args) {
  if (args.empty()) {
    out_ << "Usage: book <SYMBOL> [DEPTH]\n";
    return;
  }
  const std::string symbol = toUpper(args[0]);
  std::size_t depth = kDefaultDepth;
  if (args.size() > 1) {
    const auto parsed = parseInteger<std::size_t>(args[1]);
    if (!parsed || *parsed == 0) {
      out_ << "Error: invalid depth '" << args[1] << "'\n";
      return;
    }
    depth = *parsed;
  }
  const auto snap = exchange_.snapshot(symbol, depth);
  if (!snap) {
    out_ << "Unknown symbol '" << symbol << "'\n";
    return;
  }

  out_ << "=== " << symbol << " order book (top " << depth << ") ===\n";
  out_ << "  side        price     quantity  orders\n";
  // Asks are printed highest first so the ladder reads top-down to the spread.
  for (auto it = snap->asks.rbegin(); it != snap->asks.rend(); ++it) {
    out_ << "  ASK  " << std::setw(12) << formatPrice(it->price) << ' ' << std::setw(12)
         << it->quantity << ' ' << std::setw(7) << it->orderCount << '\n';
  }
  if (!snap->bids.empty() && !snap->asks.empty()) {
    out_ << "  ------------ spread " << formatPrice(snap->asks.front().price - snap->bids.front().price)
         << " ------------\n";
  } else {
    out_ << "  ----------------------------------------\n";
  }
  for (const LevelInfo& level : snap->bids) {
    out_ << "  BID  " << std::setw(12) << formatPrice(level.price) << ' ' << std::setw(12)
         << level.quantity << ' ' << std::setw(7) << level.orderCount << '\n';
  }
  if (snap->bids.empty() && snap->asks.empty()) {
    out_ << "  (empty)\n";
  }
}

void CommandProcessor::showOrders(const Args& args) {
  if (args.empty()) {
    out_ << "Usage: orders <SYMBOL>\n";
    return;
  }
  const std::string symbol = toUpper(args[0]);
  if (!exchange_.hasSymbol(symbol)) {
    out_ << "Unknown symbol '" << symbol << "'\n";
    return;
  }
  std::size_t shown = 0;
  for (const Side side : {Side::Sell, Side::Buy}) {
    for (const Order& order : exchange_.restingOrders(symbol, side)) {
      out_ << "  " << order.describe() << '\n';
      ++shown;
    }
  }
  if (shown == 0) {
    out_ << "No resting orders for " << symbol << '\n';
  }
}

void CommandProcessor::showTrades(const Args& args) {
  std::string symbol;
  std::size_t count = kDefaultTradeCount;
  for (const std::string& arg : args) {
    if (const auto n = parseInteger<std::size_t>(arg)) {
      count = *n;
    } else {
      symbol = toUpper(arg);
    }
  }
  std::vector<Trade> trades =
      symbol.empty() ? exchange_.trades().recentTrades(count) : exchange_.trades().tradesForSymbol(symbol);
  if (trades.size() > count) {
    trades.erase(trades.begin(), trades.end() - static_cast<std::ptrdiff_t>(count));
  }
  if (trades.empty()) {
    out_ << "No trades" << (symbol.empty() ? "" : " for " + symbol) << '\n';
    return;
  }
  for (const Trade& trade : trades) {
    out_ << "  " << timeOfDay(trade.timestamp) << "  " << describe(trade) << '\n';
  }
}

void CommandProcessor::showQuote(const Args& args) {
  if (args.empty()) {
    out_ << "Usage: quote <SYMBOL>\n";
    return;
  }
  const std::string symbol = toUpper(args[0]);
  const auto q = exchange_.quote(symbol);
  if (!q) {
    out_ << "Unknown symbol '" << symbol << "'\n";
    return;
  }
  out_ << symbol << "  bid " << optionalPrice(q->bidPrice) << " x " << q->bidQuantity << "  |  ask "
       << optionalPrice(q->askPrice) << " x " << q->askQuantity << '\n';
}

void CommandProcessor::showStats() {
  out_ << "  symbol        trades       volume       last       high        low       VWAP\n";
  for (const std::string& symbol : exchange_.symbols()) {
    const auto stats = exchange_.trades().statistics(symbol);
    if (!stats || stats->tradeCount == 0) {
      out_ << "  " << std::left << std::setw(10) << symbol << std::right << std::setw(10) << 0
           << "  (no trades)\n";
      continue;
    }
    out_ << "  " << std::left << std::setw(10) << symbol << std::right << std::setw(10)
         << stats->tradeCount << std::setw(13) << stats->volume << std::setw(11)
         << formatPrice(stats->lastPrice) << std::setw(11) << formatPrice(stats->highPrice)
         << std::setw(11) << formatPrice(stats->lowPrice) << std::setw(11) << money(stats->vwap())
         << '\n';
  }
}

void CommandProcessor::showSymbols() {
  const auto symbols = exchange_.symbols();
  out_ << "Symbols (" << symbols.size() << "):";
  for (const std::string& symbol : symbols) {
    out_ << ' ' << symbol;
  }
  out_ << '\n';
}

void CommandProcessor::addSymbol(const Args& args) {
  if (args.empty()) {
    out_ << "Usage: addsymbol <SYMBOL>\n";
    return;
  }
  const std::string symbol = toUpper(args[0]);
  if (exchange_.addSymbol(symbol)) {
    out_ << "Added symbol " << symbol << '\n';
  } else {
    out_ << "Cannot add '" << symbol << "': invalid or already listed\n";
  }
}

void CommandProcessor::exportTrades(const Args& args) {
  if (args.empty()) {
    out_ << "Usage: export <FILE.csv>\n";
    return;
  }
  const auto trades = exchange_.trades().allTrades();
  writeTradesCsv(args[0], trades);
  out_ << "Wrote " << trades.size() << " trades to " << args[0] << '\n';
}

}  // namespace mx::cli
