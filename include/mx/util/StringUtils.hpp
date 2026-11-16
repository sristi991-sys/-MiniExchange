#pragma once

#include <cctype>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace mx {

[[nodiscard]] inline std::string_view trim(std::string_view text) noexcept {
  const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!text.empty() && isSpace(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && isSpace(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

/// Splits on every delimiter, keeping empty fields ("a,,b," -> {"a","","b",""}).
[[nodiscard]] inline std::vector<std::string_view> split(std::string_view text, char delimiter) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t pos = text.find(delimiter, start);
    if (pos == std::string_view::npos) {
      parts.push_back(text.substr(start));
      return parts;
    }
    parts.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
}

/// Splits on runs of whitespace, dropping empty tokens.
[[nodiscard]] inline std::vector<std::string> tokenize(std::string_view text) {
  std::vector<std::string> tokens;
  std::size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) {
      ++i;
    }
    const std::size_t start = i;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0) {
      ++i;
    }
    if (i > start) {
      tokens.emplace_back(text.substr(start, i - start));
    }
  }
  return tokens;
}

[[nodiscard]] inline std::string toUpper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

/// Strict integer parsing: the whole string must be a valid number in range.
template <typename Int>
[[nodiscard]] std::optional<Int> parseInteger(std::string_view text) noexcept {
  static_assert(std::is_integral_v<Int>, "parseInteger requires an integral type");
  if (text.empty()) {
    return std::nullopt;
  }
  Int value{};
  const char* first = text.data();
  const char* last = first + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last) {
    return std::nullopt;
  }
  return value;
}

}  // namespace mx
