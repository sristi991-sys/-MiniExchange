#pragma once

/// @file PriceLevel.hpp
/// A FIFO queue of resting orders at one price, implemented as an intrusive
/// doubly linked list so that append, pop-front and removal from the middle
/// (cancel) are all O(1) and allocation-free.

#include "mx/core/Order.hpp"

#include <cstddef>

namespace mx {

class PriceLevel;

/// Book-internal wrapper around a resting order. The list hooks live here, not
/// in Order, so that Order stays a plain value type that is safe to copy.
struct OrderNode {
  explicit OrderNode(const Order& restingOrder) : order(restingOrder) {}

  OrderNode(const OrderNode&) = delete;
  OrderNode& operator=(const OrderNode&) = delete;

  Order order;
  OrderNode* prev{nullptr};
  OrderNode* next{nullptr};
  PriceLevel* level{nullptr};
};

class PriceLevel {
 public:
  explicit PriceLevel(Price price) noexcept : price_(price) {}

  PriceLevel(const PriceLevel&) = delete;
  PriceLevel& operator=(const PriceLevel&) = delete;

  [[nodiscard]] Price price() const noexcept { return price_; }
  [[nodiscard]] Quantity totalQuantity() const noexcept { return totalQuantity_; }
  [[nodiscard]] std::size_t orderCount() const noexcept { return orderCount_; }
  [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

  /// Oldest order at this price: the next one to trade (time priority).
  [[nodiscard]] OrderNode* front() const noexcept { return head_; }
  [[nodiscard]] OrderNode* back() const noexcept { return tail_; }

  /// Appends a node at the back of the queue (newest order, lowest time priority).
  void pushBack(OrderNode* node) noexcept {
    node->prev = tail_;
    node->next = nullptr;
    node->level = this;
    if (tail_ != nullptr) {
      tail_->next = node;
    } else {
      head_ = node;
    }
    tail_ = node;
    totalQuantity_ += node->order.remainingQuantity();
    ++orderCount_;
  }

  /// Unlinks a node from anywhere in the queue.
  void remove(OrderNode* node) noexcept {
    if (node->prev != nullptr) {
      node->prev->next = node->next;
    } else {
      head_ = node->next;
    }
    if (node->next != nullptr) {
      node->next->prev = node->prev;
    } else {
      tail_ = node->prev;
    }
    totalQuantity_ -= node->order.remainingQuantity();
    --orderCount_;
    node->prev = nullptr;
    node->next = nullptr;
    node->level = nullptr;
  }

  /// Keeps the aggregate quantity in sync after a resting order was partially filled.
  void reduceQuantity(Quantity quantity) noexcept { totalQuantity_ -= quantity; }

  template <typename Fn>
  void forEach(Fn&& fn) const {
    for (const OrderNode* node = head_; node != nullptr; node = node->next) {
      fn(node->order);
    }
  }

 private:
  Price price_;
  OrderNode* head_{nullptr};
  OrderNode* tail_{nullptr};
  Quantity totalQuantity_{0};
  std::size_t orderCount_{0};
};

}  // namespace mx
