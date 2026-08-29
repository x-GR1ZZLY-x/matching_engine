#include"order_repository.hpp"
#include"exceptions.hpp"
#include"sql_batch_insert.hpp"
#include<map>
#include<optional>
#include<string>
#include<utility>

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

// Доменное правило из docs/plan.md (3a): у рыночной заявки (пустая цена)
// активных статусов быть не может — она попала бы в частичный индекс
// idx_orders_active_sequence, loadActive() выбрала бы её и упала на пустой
// цене при следующем старте приложения. Лучше отказать здесь, пока ошибку
// ещё видно. Общая проверка для save() и saveBatch().
void validateOrderChange(const OrderChange& change){
    if(!change.price &&
        (change.status == OrderStatus::Open || change.status == OrderStatus::PartiallyFilled)){
        throw DatabaseError("Market order " + std::to_string(change.id) +
            " (no price) cannot have an active status (OPEN/PARTIALLY_FILLED)");
    }
}

// Один ряд значений upsert'а orders, в порядке колонок VALUES. Общий для
// save() (7 плейсхолдеров) и saveBatch() (те же 7 значений на каждую строку
// многострочного VALUES).
std::vector<std::optional<std::string>> orderChangeToParams(const OrderChange& change){
    return {
        std::to_string(change.id),
        Order::sideToString(change.side),
        change.price ? std::optional<std::string>(std::to_string(*change.price)) : std::nullopt,
        std::to_string(change.initialQuantity),
        std::to_string(change.remainingQuantity),
        Order::statusToString(change.status),
        std::to_string(change.sequenceNumber)
    };
}

constexpr const char* kOrderUpsertColumns =
    "(order_id, side, price, initial_quantity, remaining_quantity, status, sequence_number)";
constexpr const char* kOrderUpsertOnConflict =
    "ON CONFLICT (order_id) DO UPDATE SET "
    "side = EXCLUDED.side, "
    "price = EXCLUDED.price, "
    "initial_quantity = EXCLUDED.initial_quantity, "
    "remaining_quantity = EXCLUDED.remaining_quantity, "
    "status = EXCLUDED.status, "
    "sequence_number = EXCLUDED.sequence_number";

}

void OrderRepository::save(PgConnection& connection, const OrderChange& change){
    validateOrderChange(change);

    std::vector<std::optional<std::string>> params = orderChangeToParams(change);

    // Выполняется на каждой изменяющей команде — подготовленный запрос
    // (задача 12) экономит повторный разбор и планирование этого upsert'а.
    connection.executePrepared(
        "order_repository_upsert",
        std::string("INSERT INTO orders ") + kOrderUpsertColumns +
        " VALUES ($1, $2, $3, $4, $5, $6, $7) " + kOrderUpsertOnConflict,
        params);
}

void OrderRepository::saveBatch(PgConnection& connection, const std::vector<OrderChange>& changes){
    if(changes.empty()){
        return;
    }

    // Свёртка по order_id, оставляя последнюю запись — см. докстроку в
    // заголовке. std::map даёт детерминированный порядок строк VALUES
    // (по возрастанию order_id); порядок среди разных order_id для
    // корректности не важен, каждая строка обновляет свою собственную
    // строку orders независимо от прочих.
    //
    // validateOrderChange проверяется здесь, для каждого исходного change —
    // до свёртки, а не после. Иначе промежуточный снимок одного order_id
    // (например, старый снимок MODIFY, вытесненный из latest новым) прошёл бы
    // без проверки, хотя штатный save() проверяет каждое изменение (задача
    // 12, правка 3).
    std::map<int, OrderChange> latest;
    for(const auto& change : changes){
        validateOrderChange(change);
        latest[change.id] = change;
    }

    std::vector<OrderChange> rows;
    rows.reserve(latest.size());
    for(auto& [id, change] : latest){
        rows.push_back(std::move(change));
    }

    // Резка на несколько execute() при превышении предела параметров
    // протокола и построение плейсхолдеров — общие для трёх репозиториев,
    // см. sql_batch_insert.hpp. Обычный execute() внутри, не
    // executePrepared() — форма запроса зависит от числа строк после
    // свёртки (задача 12, "Доменные правила", п.6). Значения по-прежнему
    // идут отдельным массивом параметров, в текст запроса подставлены
    // только номера плейсхолдеров.
    executeBatchedInsert(connection, rows.size(), 7,
        std::string("INSERT INTO orders ") + kOrderUpsertColumns + " VALUES ",
        std::string(" ") + kOrderUpsertOnConflict,
        [&rows](std::size_t row){ return orderChangeToParams(rows[row]); });
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

long long OrderRepository::maxSequenceNumber(PgConnection& connection){
    // Без WHERE — по всей таблице: loadActive() фильтрует по статусу и
    // поэтому не годится для этого запроса (см. комментарий в заголовке).
    PgResult result = connection.execute("SELECT MAX(sequence_number) FROM orders");

    // MAX() над пустой таблицей возвращает одну строку с NULL — штатный
    // случай первого запуска, счётчик тогда остаётся на начальном значении.
    // SELECT MAX(...) без GROUP BY всегда возвращает ровно одну строку,
    // поэтому проверять result.rowCount() == 0 незачем.
    if(result.isNull(0, 0)){
        return 0;
    }

    return parseLongLong(result.getValue(0, 0), "orders.sequence_number (MAX)");
}

}
