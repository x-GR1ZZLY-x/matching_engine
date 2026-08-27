#include"order_repository.hpp"
#include"exceptions.hpp"
#include<optional>
#include<string>

namespace matching_engine{

namespace{

// std::stoi/std::stoll бросают std::invalid_argument/std::out_of_range —
// не входят в иерархию MatchingEngineError, и ничего не сообщают о том, что
// строка пришла из БД. Application ловит только MatchingEngineError, поэтому
// такое исключение дошло бы до std::terminate без ERROR: и без записи в лог.
int parseInt(const std::string& text, const std::string& context){
    try{
        return std::stoi(text);
    } catch(const std::exception&){
        throw DatabaseError("Failed to parse integer from database (" + context +
            "): '" + text + "'");
    }
}

long long parseLongLong(const std::string& text, const std::string& context){
    try{
        return std::stoll(text);
    } catch(const std::exception&){
        throw DatabaseError("Failed to parse integer from database (" + context +
            "): '" + text + "'");
    }
}

}

void OrderRepository::save(PgConnection& connection, const OrderChange& change){
    // Доменное правило из docs/plan.md (3a): у рыночной заявки (пустая цена)
    // активных статусов быть не может — она попала бы в частичный индекс
    // idx_orders_active_sequence, loadActive() выбрала бы её и упала на
    // пустой цене при следующем старте приложения. Лучше отказать здесь,
    // пока ошибку ещё видно.
    if(!change.price &&
        (change.status == OrderStatus::Open || change.status == OrderStatus::PartiallyFilled)){
        throw DatabaseError("Market order " + std::to_string(change.id) +
            " (no price) cannot have an active status (OPEN/PARTIALLY_FILLED)");
    }

    std::vector<std::optional<std::string>> params{
        std::to_string(change.id),
        Order::sideToString(change.side),
        change.price ? std::optional<std::string>(std::to_string(*change.price)) : std::nullopt,
        std::to_string(change.initialQuantity),
        std::to_string(change.remainingQuantity),
        Order::statusToString(change.status),
        std::to_string(change.sequenceNumber)
    };

    connection.execute(
        "INSERT INTO orders "
        "(order_id, side, price, initial_quantity, remaining_quantity, status, sequence_number) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7) "
        "ON CONFLICT (order_id) DO UPDATE SET "
        "side = EXCLUDED.side, "
        "price = EXCLUDED.price, "
        "initial_quantity = EXCLUDED.initial_quantity, "
        "remaining_quantity = EXCLUDED.remaining_quantity, "
        "status = EXCLUDED.status, "
        "sequence_number = EXCLUDED.sequence_number",
        params);
}

std::vector<std::shared_ptr<Order>> OrderRepository::loadActive(PgConnection& connection){
    // Условие статуса — константы схемы, а не параметр запроса: сравнивать
    // не с чем, значение никуда не подставляется, конкатенации нет.
    PgResult result = connection.execute(
        "SELECT order_id, side, price, initial_quantity, remaining_quantity, "
        "status, sequence_number FROM orders "
        "WHERE status IN ('OPEN', 'PARTIALLY_FILLED') "
        "ORDER BY sequence_number ASC");

    std::vector<std::shared_ptr<Order>> orders;
    orders.reserve(static_cast<size_t>(result.rowCount()));

    for(int row = 0; row < result.rowCount(); ++row){
        // Активная заявка обязана иметь цену — NULL здесь означает
        // повреждённые данные (рыночные заявки активными не бывают).
        std::string orderIdText = result.getValue(row, 0);

        if(result.isNull(row, 2)){
            throw DatabaseError("Active order " + orderIdText +
                " has a NULL price in the database");
        }

        int id = parseInt(orderIdText, "orders.order_id, row " + std::to_string(row));
        int price = parseInt(result.getValue(row, 2),
            "orders.price for order " + orderIdText);
        int initialQuantity = parseInt(result.getValue(row, 3),
            "orders.initial_quantity for order " + orderIdText);
        int remainingQuantity = parseInt(result.getValue(row, 4),
            "orders.remaining_quantity for order " + orderIdText);
        long long sequenceNumber = parseLongLong(result.getValue(row, 6),
            "orders.sequence_number for order " + orderIdText);

        // sideFromString/statusFromString бросают OrderError, как и
        // восстанавливающий конструктор ниже — весь разбор строки под одним
        // try, иначе испорченное значение side/status дало бы исключение без
        // указания, что данные пришли из БД и от какой заявки.
        try{
            Side side = Order::sideFromString(result.getValue(row, 1));
            OrderStatus status = Order::statusFromString(result.getValue(row, 5));

            // Восстанавливающий конструктор: сохраняет номер последовательности,
            // исходное количество и статус как есть, SequenceGenerator не трогает.
            orders.push_back(std::make_shared<Order>(id, side, price, remainingQuantity,
                initialQuantity, sequenceNumber, status));
        } catch(const OrderError& e){
            throw DatabaseError("Corrupted order row in database (order_id=" +
                orderIdText + "): " + e.what());
        }
    }

    return orders;
}

}
