#include "mx/core/Types.hpp"
#include "mx/util/StringUtils.hpp"

#include <gtest/gtest.h>

using namespace mx;

TEST(PriceParsing, ParsesWholeAndFractionalPrices) {
  EXPECT_EQ(parsePrice("150"), Price{15000});
  EXPECT_EQ(parsePrice("150.2"), Price{15020});
  EXPECT_EQ(parsePrice("150.25"), Price{15025});
  EXPECT_EQ(parsePrice("0.01"), Price{1});
  EXPECT_EQ(parsePrice(".5"), Price{50});
  EXPECT_EQ(parsePrice("7."), Price{700});
  EXPECT_EQ(parsePrice("0"), Price{0});
}

TEST(PriceParsing, RejectsMalformedInput) {
  for (const char* text : {"", ".", "abc", "1.234", "-5", "+5", "1.2.3", "12a", "1e5", " 1"}) {
    EXPECT_FALSE(parsePrice(text).has_value()) << "input: '" << text << "'";
  }
}

TEST(PriceParsing, RejectsOverflow) {
  EXPECT_FALSE(parsePrice("99999999999999999999").has_value());
  EXPECT_TRUE(parsePrice("1000000000").has_value());
}

TEST(PriceFormatting, AlwaysUsesTwoDecimals) {
  EXPECT_EQ(formatPrice(15025), "150.25");
  EXPECT_EQ(formatPrice(15000), "150.00");
  EXPECT_EQ(formatPrice(5), "0.05");
  EXPECT_EQ(formatPrice(0), "0.00");
  EXPECT_EQ(formatPrice(-150), "-1.50");
}

TEST(PriceFormatting, RoundTripsThroughParse) {
  for (const Price price : {Price{1}, Price{99}, Price{100}, Price{12345}, Price{99999999}}) {
    EXPECT_EQ(parsePrice(formatPrice(price)), price);
  }
}

TEST(EnumStrings, RoundTripCaseInsensitively) {
  EXPECT_EQ(parseSide("buy"), Side::Buy);
  EXPECT_EQ(parseSide("S"), Side::Sell);
  EXPECT_EQ(parseOrderType("mkt"), OrderType::Market);
  EXPECT_EQ(parseOrderType("Limit"), OrderType::Limit);
  EXPECT_EQ(parseTimeInForce("ioc"), TimeInForce::IOC);
  EXPECT_FALSE(parseTimeInForce("DAY").has_value());
  for (const auto status : {OrderStatus::New, OrderStatus::PartiallyFilled, OrderStatus::Filled,
                            OrderStatus::Cancelled, OrderStatus::Rejected}) {
    EXPECT_EQ(parseOrderStatus(toString(status)), status);
  }
  EXPECT_EQ(opposite(Side::Buy), Side::Sell);
  EXPECT_EQ(opposite(Side::Sell), Side::Buy);
}

TEST(Identifiers, SymbolValidation) {
  EXPECT_TRUE(isValidSymbol("AAPL"));
  EXPECT_TRUE(isValidSymbol("BRK.B"));
  EXPECT_TRUE(isValidSymbol("NIFTY_50"));
  EXPECT_FALSE(isValidSymbol(""));
  EXPECT_FALSE(isValidSymbol("aapl"));
  EXPECT_FALSE(isValidSymbol("A,B"));
  EXPECT_FALSE(isValidSymbol("THIS_SYMBOL_IS_TOO_LONG"));
}

TEST(Identifiers, TraderValidation) {
  EXPECT_TRUE(isValidTrader(""));
  EXPECT_TRUE(isValidTrader("alice_01"));
  EXPECT_FALSE(isValidTrader("bob smith"));
  EXPECT_FALSE(isValidTrader("x,y"));
  EXPECT_FALSE(isValidTrader(std::string(40, 'a')));
}

TEST(StringUtils, SplitKeepsEmptyFields) {
  const auto parts = split("a,,b,", ',');
  ASSERT_EQ(parts.size(), 4u);
  EXPECT_EQ(parts[0], "a");
  EXPECT_EQ(parts[1], "");
  EXPECT_EQ(parts[2], "b");
  EXPECT_EQ(parts[3], "");
}

TEST(StringUtils, TokenizeAndParseInteger) {
  const auto tokens = tokenize("  buy  AAPL\t100 ");
  ASSERT_EQ(tokens.size(), 3u);
  EXPECT_EQ(tokens[2], "100");
  EXPECT_EQ(parseInteger<int>("42"), 42);
  EXPECT_FALSE(parseInteger<unsigned>("-1").has_value());
  EXPECT_FALSE(parseInteger<int>("12x").has_value());
  EXPECT_FALSE(parseInteger<int>("").has_value());
  EXPECT_EQ(toUpper("msft"), "MSFT");
  EXPECT_EQ(trim("  x \r\n"), "x");
}
