#pragma once

/// @file ObjectPool.hpp
/// Fixed-size object pool with RAII handles.
///
/// Objects are carved out of large chunks and recycled through an intrusive
/// free list, so steady-state allocation and deallocation are O(1) pointer
/// swaps with no calls into the global heap. Handles are std::unique_ptr with a
/// custom deleter, so an object is returned to the pool automatically when its
/// owner goes away (RAII), exactly like a heap allocation would be freed.
///
/// The pool is not thread-safe; each order book owns one and is only touched
/// while that book's lock is held. The pool must outlive every handle it issued.

#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace mx {

template <typename T>
class ObjectPool {
 public:
  class Deleter {
   public:
    Deleter() noexcept = default;
    explicit Deleter(ObjectPool* pool) noexcept : pool_(pool) {}
    void operator()(T* object) const noexcept {
      if (pool_ != nullptr && object != nullptr) {
        pool_->destroy(object);
      }
    }

   private:
    ObjectPool* pool_{nullptr};
  };

  using Ptr = std::unique_ptr<T, Deleter>;

  explicit ObjectPool(std::size_t chunkSize = 1024) : chunkSize_(chunkSize == 0 ? 1 : chunkSize) {}

  ObjectPool(const ObjectPool&) = delete;
  ObjectPool& operator=(const ObjectPool&) = delete;
  ObjectPool(ObjectPool&&) = delete;
  ObjectPool& operator=(ObjectPool&&) = delete;
  ~ObjectPool() = default;

  /// Constructs a T in a recycled slot. Strong exception guarantee: if T's
  /// constructor throws, the slot goes back to the free list.
  template <typename... Args>
  [[nodiscard]] Ptr make(Args&&... args) {
    Slot* slot = acquireSlot();
    T* object = nullptr;
    try {
      object = ::new (static_cast<void*>(slot->storage)) T(std::forward<Args>(args)...);
    } catch (...) {
      releaseSlot(slot);
      throw;
    }
    ++live_;
    return Ptr(object, Deleter(this));
  }

  /// Pre-allocates capacity for at least `objects` objects.
  void reserve(std::size_t objects) {
    while (capacity() < objects) {
      grow();
    }
  }

  [[nodiscard]] std::size_t liveObjects() const noexcept { return live_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return chunks_.size() * chunkSize_; }
  [[nodiscard]] std::size_t chunkCount() const noexcept { return chunks_.size(); }
  [[nodiscard]] std::size_t chunkSize() const noexcept { return chunkSize_; }

 private:
  union Slot {
    Slot* next;
    alignas(T) unsigned char storage[sizeof(T)];
  };

  Slot* acquireSlot() {
    if (freeList_ == nullptr) {
      grow();
    }
    Slot* slot = freeList_;
    freeList_ = slot->next;
    return slot;
  }

  void releaseSlot(Slot* slot) noexcept {
    slot->next = freeList_;
    freeList_ = slot;
  }

  void destroy(T* object) noexcept {
    object->~T();
    releaseSlot(reinterpret_cast<Slot*>(object));
    --live_;
  }

  void grow() {
    // Push first so that a throwing push_back cannot leave dangling free-list entries.
    chunks_.push_back(std::make_unique<Slot[]>(chunkSize_));
    Slot* chunk = chunks_.back().get();
    for (std::size_t i = 0; i + 1 < chunkSize_; ++i) {
      chunk[i].next = &chunk[i + 1];
    }
    chunk[chunkSize_ - 1].next = freeList_;
    freeList_ = chunk;
  }

  std::size_t chunkSize_;
  std::vector<std::unique_ptr<Slot[]>> chunks_;
  Slot* freeList_{nullptr};
  std::size_t live_{0};
};

}  // namespace mx
