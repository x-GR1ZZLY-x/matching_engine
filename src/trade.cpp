#include "trade.hpp"

namespace matching_engine{

Trade::Trade(int buyOrderId, int sellOrderId, int price, int quantity)
    : buyOrderId_(buyOrderId),
    sellOrderId_(sellOrderId),
    price_(price),
    quantity_(quantity)
    {}
}
