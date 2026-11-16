#include "mx/util/ObjectPool.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

using mx::ObjectPool;

namespace {

int gAlive = 0;

struct Tracked {
  explicit Tracked(int v) : value(v), label("tracked-object-with-a-long-name") { ++gAlive; }
  ~Tracked() { --gAlive; }
  Tracked(const Tracked&) = delete;
  Tracked& operator=(const Tracked&) = delete;
  int value;
  std::string label;  // Non-trivial member: verifies destructors really run.
};

struct ThrowsOnDemand {
  explicit ThrowsOnDemand(bool fail) {
    if (fail) {
      throw std::runtime_error("constructor failure");
    }
  }
};

}  // namespace

TEST(ObjectPool, ConstructsAndDestroysThroughRaiiHandles) {
  gAlive = 0;
  ObjectPool<Tracked> pool(4);
  {
    auto a = pool.make(1);
    auto b = pool.make(2);
    EXPECT_EQ(a->value, 1);
    EXPECT_EQ(b->value, 2);
    EXPECT_EQ(gAlive, 2);
    EXPECT_EQ(pool.liveObjects(), 2u);
  }
  EXPECT_EQ(gAlive, 0);
  EXPECT_EQ(pool.liveObjects(), 0u);
}

TEST(ObjectPool, ReusesFreedSlotsLifo) {
  ObjectPool<Tracked> pool(4);
  auto first = pool.make(1);
  Tracked* address = first.get();
  first.reset();
  auto second = pool.make(2);
  EXPECT_EQ(second.get(), address);
  EXPECT_EQ(second->value, 2);
}

TEST(ObjectPool, GrowsInChunksAndKeepsObjectsStable) {
  ObjectPool<Tracked> pool(8);
  std::vector<ObjectPool<Tracked>::Ptr> objects;
  std::vector<Tracked*> addresses;
  for (int i = 0; i < 100; ++i) {
    objects.push_back(pool.make(i));
    addresses.push_back(objects.back().get());
  }
  EXPECT_EQ(pool.chunkCount(), 13u);  // ceil(100 / 8)
  EXPECT_GE(pool.capacity(), 100u);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(objects[static_cast<std::size_t>(i)].get(), addresses[static_cast<std::size_t>(i)]);
    EXPECT_EQ(objects[static_cast<std::size_t>(i)]->value, i);
  }
}

TEST(ObjectPool, SteadyStateChurnDoesNotGrow) {
  ObjectPool<Tracked> pool(16);
  for (int round = 0; round < 1000; ++round) {
    std::vector<ObjectPool<Tracked>::Ptr> batch;
    for (int i = 0; i < 16; ++i) {
      batch.push_back(pool.make(i));
    }
  }
  EXPECT_EQ(pool.chunkCount(), 1u);
  EXPECT_EQ(pool.liveObjects(), 0u);
}

TEST(ObjectPool, ConstructorExceptionReturnsSlot) {
  ObjectPool<ThrowsOnDemand> pool(2);
  EXPECT_THROW((void)pool.make(true), std::runtime_error);
  EXPECT_EQ(pool.liveObjects(), 0u);
  auto a = pool.make(false);
  auto b = pool.make(false);
  EXPECT_EQ(pool.chunkCount(), 1u);  // The failed slot was recycled.
}

TEST(ObjectPool, ReservePreallocates) {
  ObjectPool<Tracked> pool(10);
  pool.reserve(35);
  EXPECT_EQ(pool.chunkCount(), 4u);
  EXPECT_EQ(pool.capacity(), 40u);
}
