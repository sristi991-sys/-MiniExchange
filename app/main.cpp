// MiniExchange interactive terminal.
//
//   miniexchange                         interactive session
//   miniexchange --script demo.txt       run commands from a file
//   miniexchange --journal mx.journal    durable session (replayed on restart)

#include "CommandProcessor.hpp"

#include "mx/exchange/Exchange.hpp"
#include "mx/util/StringUtils.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr const char* kDefaultSymbols = "AAPL,MSFT,GOOG,AMZN,TSLA";

void printUsage() {
  std::cout << "Usage: miniexchange [options]\n"
               "  --symbols A,B,C   symbols to list (default " << kDefaultSymbols << ")\n"
               "  --journal FILE    write-ahead journal; replayed on startup if it exists\n"
               "  --script FILE     run commands from FILE, then exit\n"
               "  --interactive     with --script: stay interactive after the script\n"
               "  --quiet           no banner or prompt\n"
               "  --help            show this message\n";
}

struct Options {
  std::string symbols = kDefaultSymbols;
  std::string journal;
  std::string script;
  bool interactive = false;
  bool quiet = false;
};

bool parseArgs(int argc, char** argv, Options& options) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    const auto needValue = [&](std::string& target) {
      if (i + 1 >= args.size()) {
        std::cerr << "Missing value for " << arg << '\n';
        return false;
      }
      target = args[++i];
      return true;
    };
    if (arg == "--symbols") {
      if (!needValue(options.symbols)) return false;
    } else if (arg == "--journal") {
      if (!needValue(options.journal)) return false;
    } else if (arg == "--script") {
      if (!needValue(options.script)) return false;
    } else if (arg == "--interactive") {
      options.interactive = true;
    } else if (arg == "--quiet" || arg == "-q") {
      options.quiet = true;
    } else if (arg == "--help" || arg == "-h") {
      printUsage();
      std::exit(0);
    } else {
      std::cerr << "Unknown option '" << arg << "'\n";
      printUsage();
      return false;
    }
  }
  return true;
}

/// Feeds lines to the processor. Returns false if the user typed quit.
bool runStream(std::istream& in, mx::cli::CommandProcessor& processor, bool echo, bool prompt) {
  std::string line;
  while (true) {
    if (prompt) {
      std::cout << "mx> " << std::flush;
    }
    if (!std::getline(in, line)) {
      return true;
    }
    if (echo && !mx::trim(line).empty()) {
      std::cout << "mx> " << line << '\n';
    }
    if (!processor.execute(line)) {
      return false;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseArgs(argc, argv, options)) {
    return 2;
  }

  try {
    mx::ExchangeConfig config;
    config.journalPath = options.journal;
    config.flushJournalOnWrite = true;  // Interactive use: durability over speed.
    const bool hadJournal = !options.journal.empty() && std::filesystem::exists(options.journal);

    mx::Exchange exchange(config);
    if (hadJournal) {
      const mx::RecoveryStats stats = exchange.recover(options.journal);
      std::cout << "Recovered from " << options.journal << ": " << stats.ordersReplayed
                << " orders, " << stats.cancelsReplayed << " cancels, " << stats.tradesReplayed
                << " trades";
      if (stats.malformedLines > 0) {
        std::cout << " (" << stats.malformedLines << " malformed lines skipped)";
      }
      std::cout << '\n';
    }
    for (const auto symbol : mx::split(options.symbols, ',')) {
      const std::string upper = mx::toUpper(mx::trim(symbol));
      if (!upper.empty() && !exchange.hasSymbol(upper) && !exchange.addSymbol(upper)) {
        std::cerr << "Ignoring invalid symbol '" << upper << "'\n";
      }
    }

    mx::cli::CommandProcessor processor(exchange, std::cout);

    if (!options.quiet) {
      std::cout << "MiniExchange order matching engine. Type 'help' for commands.\n";
      processor.execute("symbols");
    }

    bool keepGoing = true;
    if (!options.script.empty()) {
      std::ifstream script(options.script);
      if (!script.is_open()) {
        std::cerr << "Cannot open script '" << options.script << "'\n";
        return 1;
      }
      keepGoing = runStream(script, processor, /*echo=*/!options.quiet, /*prompt=*/false);
      if (!options.interactive) {
        return 0;
      }
    }
    if (keepGoing) {
      runStream(std::cin, processor, /*echo=*/false, /*prompt=*/!options.quiet);
    }
  } catch (const std::exception& error) {
    std::cerr << "Fatal: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
