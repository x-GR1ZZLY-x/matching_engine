#pragma once

#include<optional>
#include<vector>
#include"order.hpp"
#include"trade.hpp"

namespace matching_engine{

// Снимок состояния заявки после обработки команды. Обычная структура данных:
// ничего не знает о БД, о libpq, о SQL или об именах таблиц.
struct OrderChange{
    int id;
    Side side;
    std::optional<int> price;  // nullopt у MARKET-заявки
    int initialQuantity;
    int remainingQuantity;
    OrderStatus status;
    long long sequenceNumber;
};

// Эффект одной команды: сделки, которые она породила, и снимки всех заявок,
// которых коснулось исполнение (новая заявка, книжные заявки-контрагенты,
// отменяемая при CANCEL, изменяемая при MODIFY).
// orderChanges упорядочен по времени мутации заявки; для одного id может
// быть несколько записей (например, при MODIFY — старая и новая), и при
// применении побеждает последняя запись с этим id.
struct ExecutionResult{
    std::vector<Trade> trades;
    std::vector<OrderChange> orderChanges;
};

}
