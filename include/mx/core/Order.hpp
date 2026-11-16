#pragma once

/// @file Order.hpp
/// Order management module: client order requests, validation and the Order
/// value type with its lifecycle (New -> PartiallyFilled -> Filled/Cancelled).

#include "mx/core/Types.hpp"

#include <optional>
#include <string>

namespace mx {

/// What a client sends to the exchange. The exchange assigns the order id.
struct OrderRequest {
  std::string symbol;
  Side side{Side::Buy};
  OrderType type{OrderType::Limit};
  TimeInForce timeInForce{TimeInForce::GTC};
  Price price{0};  ///< Limit price in ticks. Ignored for market orders.
  Quantity quantity{0};
  std::string trader;

  [[nodiscard]] static OrderRequest limit(std::string symbol, Side side, Quantity quantity,
                                          Price price, TimeInForce tif = TimeInForce::GTC,
                                          std::string trader = {});
  [[nodiscard]] static OrderRequest market(std::string symbol, Side side, Quantity quantity,
                                           std::string trader = {},
                                           TimeInForce tif = TimeInForce::IOC);
};

/// Returns a human-readable reason if the request is invalid, std::nullopt otherwise.
[[nodiscard]] std::optional<std::string> validationError(const OrderRequest& request);

/// An order with an exchange-assigned id and mutable execution state.
///
/// Market orders are normalised on construction: their price is 0 and a GTC
/// time-in-force becomes IOC, because a market order can never rest in the book.
class Order {
 public:
  Order() = default;
  Order(OrderId id, std::string symbol, Side side, OrderType type, TimeInForce tif, Price price,
        Quantity quantity, std::string trader = {}, Timestamp timestamp = 0);
  Order(OrderId id, const OrderRequest& request, Timestamp timestamp = 0);

  [[nodiscard]] OrderId id() const noexcept { return id_; }
  [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }
  [[nodiscard]] const std::string& trader() const noexcept { return trader_; }
  [[nodiscard]] Side side() const noexcept { return side_; }
  [[nodiscard]] OrderType type() const noexcept { return type_; }
  [[nodiscard]] TimeInForce timeInForce() const noexcept { return tif_; }
  [[nodiscard]] OrderStatus status() const noexcept { return status_; }
  [[nodiscard]] Price price() const noexcept { return price_; }
  [[nodiscard]] Quantity quantity() const noexcept { return quantity_; }
  [[nodiscard]] Quantity remainingQuantity() const noexcept { return remaining_; }
  [[nodiscard]] Quantity filledQuantity() const noexcept { return quantity_ - remaining_; }
  [[nodiscard]] Timestamp timestamp() const noexcept { return timestamp_; }

  [[nodiscard]] bool isBuy() const noexcept { return side_ == Side::Buy; }
  [[nodiscard]] bool isLimit() const noexcept { return type_ == OrderType::Limit; }
  [[nodiscard]] bool isMarket() const noexcept { return type_ == OrderType::Market; }
  [[nodiscard]] bool isFilled() const noexcept { return remaining_ == 0; }
  /// True while the order can still trade (New or PartiallyFilled).
  [[nodiscard]] bool isActive() const noexcept {
    return status_ == OrderStatus::New || status_ == OrderStatus::PartiallyFilled;
  }

  /// Executes `quantity` of the order.
  /// @throws std::logic_error if the order is no longer active.
  /// @throws std::invalid_argument if quantity is 0 or exceeds the remaining quantity.
  void fill(Quantity quantity);

  /// Cancels an active order. The remaining quantity is kept so callers can
  /// report how much was cancelled. No-op for inactive orders.
  void cancel() noexcept;

  /// Marks the order as rejected. No-op for orders that already traded.
  void reject() noexcept;

  [[nodiscard]] std::string describe() const;

 private:
  OrderId id_{kInvalidOrderId};
  std::string symbol_;
  std::string trader_;
  Price price_{0};
  Quantity quantity_{0};
  Quantity remaining_{0};
  Timestamp timestamp_{0};
  Side side_{Side::Buy};
  OrderType type_{OrderType::Limit};
  TimeInForce tif_{TimeInForce::GTC};
  OrderStatus status_{OrderStatus::New};
};

}  // namespace mx
