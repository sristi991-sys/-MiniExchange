#pragma once

/// @file CommandProcessor.hpp
/// Text command interpreter for the interactive exchange terminal.

#include "mx/exchange/Exchange.hpp"

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace mx::cli {

class CommandProcessor {
 public:
  CommandProcessor(Exchange& exchange, std::ostream& out);

  /// Executes one command line. Returns false when the user asked to quit.
  bool execute(std::string_view line);

  [[nodiscard]] static std::string helpText();

 private:
  using Args = std::vector<std::string>;

  void placeOrder(Side side, const Args& args);
  void cancel(const Args& args);
  void showBook(const Args& args);
  void showOrders(const Args& args);
  void showTrades(const Args& args);
  void showQuote(const Args& args);
  void showStats();
  void showSymbols();
  void addSymbol(const Args& args);
  void exportTrades(const Args& args);
  void printReport(const ExecutionReport& report);

  Exchange& exchange_;
  std::ostream& out_;
};

}  // namespace mx::cli
