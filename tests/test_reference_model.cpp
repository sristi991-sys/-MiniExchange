// Differential (model-based) test: the optimised engine must produce exactly
// the same trades and book state as a deliberately naive reference model for
// long random order streams.

#include "mx/matching/MatchingEngine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

using namespace mx;

namespace {

struct RefTrade {
  OrderId buy;
  OrderId sell;
  Price price;
  Quantity quantity;
  bool operator==(const RefTrade& o) const {
    return buy == o.buy && sell == o.sell && price == o.price && quantity == o.quantity;
  }
};

/// O(n) per operation, obviously-correct price-time priority.
class ReferenceBook {
 public:
  std::vector<RefTrade> submit(OrderId id, Side side, OrderType type, TimeInForce tif, Price price,
                               Quantity quantity) {
    std::vector<RefTrade> trades;
    const auto crosses = [&](const Resting& r) {
      if (r.side == side) return false;
      if (type == OrderType::Market) return true;
      return side == Side::Buy ? r.price <= price : r.price >= price;
    };
    const auto better = [&](const Resting& a, const Resting& b) {
      if (a.price != b.price) return side == Side::Buy ? a.price < b.price : a.price > b.price;
      return a.sequence < b.sequence;
    };

    if (tif == TimeInForce::FOK) {
      Quantity available = 0;
      for (const Resting& r : resting_) {
        if (crosses(r)) available += r.remaining;
      }
      if (available < quantity) return trades;
    }

    Quantity remaining = quantity;
    while (remaining > 0) {
      auto best = resting_.end();
      for (auto it = resting_.begin(); it != resting_.end(); ++it) {
        if (crosses(*it) && (best == resting_.end() || better(*it, *best))) best = it;
      }
      if (best == resting_.end()) break;
      const Quantity q = std::min(remaining, best->remaining);
      trades.push_back(side == Side::Buy ? RefTrade{id, best->id, best->price, q}
                                         : RefTrade{best->id, id, best->price, q});
      remaining -= q;
      best->remaining -= q;
      if (best->remaining == 0) resting_.erase(best);
    }
    if (remaining > 0 && type == OrderType::Limit && tif == TimeInForce::GTC) {
      resting_.push_back(Resting{id, side, price, remaining, sequence_++});
    }
    return trades;
  }

  bool cancel(OrderId id) {
    const auto it = std::find_if(resting_.begin(), resting_.end(),
                                 [id](const Resting& r) { return r.id == id; });
    if (it == resting_.end()) return false;
    resting_.erase(it);
    return true;
  }

  /// Aggregated levels, best first, matching OrderBook::snapshot().
  std::vector<LevelInfo> levels(Side side) const {
    std::vector<LevelInfo> out;
    std::vector<Resting> sorted;
    for (const Resting& r : resting_) {
      if (r.side == side) sorted.push_back(r);
    }
    std::sort(sorted.begin(), sorted.end(), [side](const Resting& a, const Resting& b) {
      return side == Side::Buy ? a.price > b.price : a.price < b.price;
    });
    for (const Resting& r : sorted) {
      if (out.empty() || out.back().price != r.price) out.push_back(LevelInfo{r.price, 0, 0});
      out.back().quantity += r.remaining;
      ++out.back().orderCount;
    }
    return out;
  }

  std::size_t size() const { return resting_.size(); }

 private:
  struct Resting {
    OrderId id;
    Side side;
    Price price;
    Quantity remaining;
    std::uint64_t sequence;
  };
  std::vector<Resting> resting_;
  std::uint64_t sequence_{0};
};

std::vector<RefTrade> toRef(const std::vector<Trade>& trades) {
  std::vector<RefTrade> out;
  for (const Trade& t : trades) out.push_back(RefTrade{t.buyOrderId, t.sellOrderId, t.price, t.quantity});
  return out;
}

class ReferenceModelTest : public ::testing::TestWithParam<std::uint64_t> {};

}  // namespace

TEST_P(ReferenceModelTest, EngineMatchesNaiveModel) {
  std::mt19937_64 rng(GetParam());
  std::uniform_int_distribution<int> percent(0, 99);
  std::uniform_int_distribution<Price> price(95, 105);
  std::uniform_int_distribution<Quantity> quantity(1, 50);

  MatchingEngine engine("REF");
  ReferenceBook reference;
  std::vector<OrderId> submitted;
  OrderId nextId = 1;
  std::size_t totalTrades = 0;

  for (int step = 0; step < 4000; ++step) {
    const int roll = percent(rng);
    if (roll < 15 && !submitted.empty()) {
      const OrderId target =
          submitted[std::uniform_int_distribution<std::size_t>(0, submitted.size() - 1)(rng)];
      ASSERT_EQ(engine.cancel(target).success, reference.cancel(target)) << "step " << step;
      continue;
    }
    const Side side = percent(rng) < 50 ? Side::Buy : Side::Sell;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    if (roll < 25) {
      type = OrderType::Market;
      tif = percent(rng) < 20 ? TimeInForce::FOK : TimeInForce::IOC;
    } else if (roll < 35) {
      tif = TimeInForce::IOC;
    } else if (roll < 42) {
      tif = TimeInForce::FOK;
    }
    const Price p = type == OrderType::Market ? 0 : price(rng);
    const Quantity q = quantity(rng);
    const OrderId id = nextId++;
    submitted.push_back(id);

    const ExecutionReport report = engine.process(Order(id, "REF", side, type, tif, p, q));
    const std::vector<RefTrade> expected = reference.submit(id, side, type, tif, p, q);
    ASSERT_EQ(toRef(report.trades), expected) << "step " << step << " order " << id;
    totalTrades += expected.size();
    ASSERT_EQ(report.quantity,
              report.filledQuantity + report.restingQuantity + report.cancelledQuantity);
    ASSERT_EQ(engine.book().orderCount(), reference.size()) << "step " << step;

    // The book can never be crossed after matching completes.
    const auto bid = engine.book().bestBid();
    const auto ask = engine.book().bestAsk();
    if (bid && ask) {
      ASSERT_LT(*bid, *ask) << "crossed book at step " << step;
    }
  }

  EXPECT_GT(totalTrades, 1000u) << "workload too passive to exercise matching";
  const BookSnapshot snap = engine.book().snapshot();
  EXPECT_EQ(snap.bids, reference.levels(Side::Buy));
  EXPECT_EQ(snap.asks, reference.levels(Side::Sell));
}

INSTANTIATE_TEST_SUITE_P(RandomSeeds, ReferenceModelTest,
                         ::testing::Values(1ULL, 7ULL, 42ULL, 1234ULL, 99999ULL));
