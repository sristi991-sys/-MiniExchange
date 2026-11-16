#include "mx/core/Order.hpp"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace mx {

OrderRequest OrderRequest::limit(std::string symbol, Side side, Quantity quantity, Price price,
                                 TimeInForce tif, std::string trader) {
  OrderRequest request;
  request.symbol = std::move(symbol);
  request.side = side;
  request.type = OrderType::Limit;
  request.timeInForce = tif;
  request.price = price;
  request.quantity = quantity;
  request.trader = std::move(trader);
  return request;
}

OrderRequest OrderRequest::market(std::string symbol, Side side, Quantity quantity,
                                  std::string trader, TimeInForce tif) {
  OrderRequest request;
  request.symbol = std::move(symbol);
  request.side = side;
  request.type = OrderType::Market;
  request.timeInForce = tif;
  request.price = 0;
  request.quantity = quantity;
  request.trader = std::move(trader);
  return request;
}

std::optional<std::string> validationError(const OrderRequest& request) {
  if (!isValidSymbol(request.symbol)) {
    return "invalid symbol '" + request.symbol + "'";
  }
  if (!isValidTrader(request.trader)) {
    return "invalid trader id '" + request.trader + "'";
  }
  if (request.quantity == 0) {
    return std::string("quantity must be positive");
  }
  if (request.quantity > kMaxOrderQuantity) {
    return "quantity exceeds the maximum of " + std::to_string(kMaxOrderQuantity);
  }
  if (request.type == OrderType::Limit) {
    if (request.price <= 0) {
      return std::string("limit price must be positive");
    }
    if (request.price > kMaxPrice) {
      return "limit price exceeds the maximum of " + formatPrice(kMaxPrice);
    }
  }
  return std::nullopt;
}

Order::Order(OrderId id, std::string symbol, Side side, OrderType type, TimeInForce tif,
             Price price, Quantity quantity, std::string trader, Timestamp timestamp)
    : id_(id),
      symbol_(std::move(symbol)),
      trader_(std::move(trader)),
      price_(type == OrderType::Market ? 0 : price),
      quantity_(quantity),
      remaining_(quantity),
      timestamp_(timestamp),
      side_(side),
      type_(type),
      tif_(type == OrderType::Market && tif == TimeInForce::GTC ? TimeInForce::IOC : tif) {}

Order::Order(OrderId id, const OrderRequest& request, Timestamp timestamp)
    : Order(id, request.symbol, request.side, request.type, request.timeInForce, request.price,
            request.quantity, request.trader, timestamp) {}

void Order::fill(Quantity quantity) {
  if (!isActive()) {
    throw std::logic_error("cannot fill order " + std::to_string(id_) + " in state " +
                           std::string(toString(status_)));
  }
  if (quantity == 0 || quantity > remaining_) {
    throw std::invalid_argument("invalid fill quantity " + std::to_string(quantity) +
                                " for order " + std::to_string(id_) + " with " +
                                std::to_string(remaining_) + " remaining");
  }
  remaining_ -= quantity;
  status_ = remaining_ == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
}

void Order::cancel() noexcept {
  if (isActive()) {
    status_ = OrderStatus::Cancelled;
  }
}

void Order::reject() noexcept {
  if (status_ == OrderStatus::New) {
    status_ = OrderStatus::Rejected;
  }
}

std::string Order::describe() const {
  std::ostringstream out;
  out << '#' << id_ << ' ' << toString(side_) << ' ' << quantity_ << ' ' << symbol_ << ' ';
  if (isMarket()) {
    out << "MKT";
  } else {
    out << '@' << formatPrice(price_);
  }
  out << ' ' << toString(tif_) << " [" << toString(status_) << ", remaining " << remaining_
      << ']';
  if (!trader_.empty()) {
    out << " trader=" << trader_;
  }
  return out.str();
}

}  // namespace mx
