-- Схема ДЗ-3: заявки, сделки и обработанные команды.
-- Все операторы идемпотентны (CREATE TABLE IF NOT EXISTS), повторный запуск
-- приложения на уже созданной схеме проходит без ошибок.

-- Книга заявок. price допускает NULL — у рыночной заявки цены нет.
-- sequence_number уникален и задаёт Price-Time Priority; выдаётся приложением
-- (SequenceGenerator), монотонно возрастает и переживает перезапуск.
CREATE TABLE IF NOT EXISTS orders (
    order_id INTEGER PRIMARY KEY,
    side VARCHAR(4) NOT NULL CHECK (side IN ('BUY', 'SELL')),
    price INTEGER,
    initial_quantity INTEGER NOT NULL,
    remaining_quantity INTEGER NOT NULL,
    status VARCHAR(20) NOT NULL
        CHECK (status IN ('OPEN', 'PARTIALLY_FILLED', 'FILLED', 'CANCELLED')),
    sequence_number BIGINT NOT NULL UNIQUE
);

-- Совершённые сделки. trade_id выдаёт база (BIGSERIAL), приложение
-- идентификатор сделки не придумывает.
CREATE TABLE IF NOT EXISTS trades (
    trade_id BIGSERIAL PRIMARY KEY,
    buy_order_id INTEGER NOT NULL REFERENCES orders (order_id),
    sell_order_id INTEGER NOT NULL REFERENCES orders (order_id),
    price INTEGER NOT NULL,
    quantity INTEGER NOT NULL
);

-- Журнал обработанных команд для идемпотентности: по command_id можно
-- вернуть результат прошлого выполнения, не трогая книгу и не считая сделки
-- заново.
CREATE TABLE IF NOT EXISTS processed_commands (
    command_id TEXT PRIMARY KEY,
    command_type TEXT NOT NULL,
    status TEXT NOT NULL,
    result JSONB
);
