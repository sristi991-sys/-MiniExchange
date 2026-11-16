#include "mx/core/ExecutionReport.hpp"

#include <utility>

namespace mx {

double ExecutionReport::averageFillPrice() const noexcept {
  long double notional = 0;
  long double volume = 0;
  for (const Trade& trade : trades) {
    notional += static_cast<long double>(trade.price) * static_cast<long double>(trade.quantity);
    volume += static_cast<long double>(trade.quantity);
  }
  if (volume == 0) {
    return 0.0;
  }
  return static_cast<double>(notional / volume / static_cast<long double>(kPriceScale));
}

ExecutionReport ExecutionReport::fromOrder(const Order& order) {
  ExecutionReport report;
  report.orderId = order.id();
  report.symbol = order.symbol();
  report.trader = order.trader();
  report.side = order.side();
  report.type = order.type();
  report.timeInForce = order.timeInForce();
  report.price = order.price();
  report.quantity = order.quantity();
  report.filledQuantity = order.filledQuantity();
  report.status = order.status();
  return report;
}

ExecutionReport ExecutionReport::rejected(OrderId id, const OrderRequest& request,
                                          std::string reason) {
  ExecutionReport report;
  report.orderId = id;
  report.symbol = request.symbol;
  report.trader = request.trader;
  report.side = request.side;
  report.type = request.type;
  report.timeInForce = request.timeInForce;
  report.price = request.price;
  report.quantity = request.quantity;
  report.status = OrderStatus::Rejected;
  report.message = std::move(reason);
  return report;
}

}  // namespace mx
