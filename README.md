# Matching Engine

Упрощённый биржевой движок сопоставления заявок (Matching Engine). Программа принимает заявки на покупку и продажу финансового инструмента, хранит активные заявки в книге заявок (Order Book) и автоматически совершает сделки, когда цена покупателя пересекается с ценой продавца.

---

## Содержание

- [Принцип работы Limit Order Book](#принцип-работы-limit-order-book)
- [Price-Time Priority](#price-time-priority)
- [Архитектура](#архитектура)
- [Структуры данных](#структуры-данных)
- [Формат JSON-команд](#формат-json-команд)
- [Сборка](#сборка)
- [Запуск](#запуск)
- [Запуск тестов](#запуск-тестов)
- [Примеры работы](#примеры-работы)

---

## Принцип работы Limit Order Book

**Limit Order** — заявка с явно указанной ценой. Покупатель указывает максимальную цену, которую готов заплатить; продавец — минимальную цену, за которую готов продать.

**Книга заявок (Order Book)** хранит все активные лимитные заявки, упорядоченные по цене и времени поступления. Она состоит из двух сторон:

- **BUY (Bid)** — заявки на покупку, отсортированные по убыванию цены (лучшая — самая высокая цена)
- **SELL (Ask)** — заявки на продажу, отсортированные по возрастанию цены (лучшая — самая низкая цена)

**Сделка** совершается, когда цена лучшей SELL-заявки не превышает цену лучшей BUY-заявки, то есть когда спред пересекается:

```
BUY price >= SELL price → сделка возможна
```

**Цена сделки** определяется ценой заявки, которая уже находилась в книге на момент поступления встречной заявки.

---

## Price-Time Priority

Алгоритм сопоставления реализует приоритет **Price-Time**:

**1. Price Priority** — в первую очередь исполняются заявки с лучшей ценой:

- Для BUY: чем выше цена, тем выше приоритет
- Для SELL: чем ниже цена, тем выше приоритет

```
SELL в книге:   99 → 100 → 101   (приоритет слева направо)
BUY  в книге:  101 → 100 →  99   (приоритет слева направо)
```

**2. Time Priority** — при одинаковой цене первой исполняется заявка, поступившая раньше (FIFO):

```
Три BUY-заявки по цене 100, поступившие в порядке #10 → #20 → #30:
Приоритет: #10 > #20 > #30
```

Одна входящая заявка может породить несколько сделок подряд — движок продолжает сопоставление, пока заявка не исполнена полностью или в книге не осталось подходящих встречных заявок.

---

## Архитектура

```
main()
  │
  └─► Application
        │
        ├─► CommandParser      парсинг JSON → объекты Command
        │
        ├─► MatchingEngine     бизнес-логика (не знает о JSON и stdout)
        │       │
        │       └─► OrderBook  хранение активных заявок
        │
        ├─► ReportPrinter      единственный класс, пишущий в stdout
        │
        └─► Logger             Singleton-обёртка над spdlog
```

### Описание классов

| Класс | Ответственность |
|---|---|
| `Application` | Читает `argv`, оркестрирует парсинг, выполнение команд и вывод результатов |
| `CommandParser` | Валидирует JSON, проверяет поля и типы, создаёт объекты `Command` |
| `Command` / `AddCommand` / `CancelCommand` / `PrintCommand` | Иерархия команд — передают данные от парсера к движку |
| `MatchingEngine` | Принимает команды, запускает алгоритм сопоставления, хранит историю сделок |
| `OrderBook` | Хранит активные заявки с поддержкой Price-Time Priority |
| `Order` | Одна биржевая заявка; валидирует себя при создании |
| `Trade` | Результат сделки: `buyOrderId`, `sellOrderId`, `price`, `quantity` |
| `ReportPrinter` | Выводит сделки, состояние книги и ошибки |
| `Logger` | Singleton Мейерса над spdlog; остальные компоненты не включают `spdlog.h` |

**Принцип разделения ответственности:** `MatchingEngine` не парсит JSON, не читает `argc`/`argv`, не пишет в консоль и не обращается к spdlog напрямую. `ReportPrinter` — единственный класс с `std::cout`.

---

## Структуры данных

### Хранилище заявок в OrderBook

Для хранения заявок выбрана двухуровневая структура:

```cpp
std::map<int, std::deque<std::shared_ptr<Order>>> buyLevels_;
std::map<int, std::deque<std::shared_ptr<Order>>> sellLevels_;
std::unordered_map<int, std::shared_ptr<Order>>   ordersById_;
```

**Почему `std::map<price, deque>`?**

- `std::map` хранит ключи (цены) в отсортированном порядке и не требует `std::sort` после каждой вставки. Вставка и удаление — O(log N).
- Для BUY лучшая цена — максимальная: итерируемся через `rbegin()`.
- Для SELL лучшая цена — минимальная: итерируемся через `begin()`.
- Пустые ценовые уровни удаляются из `map` автоматически.

**Почему `std::deque` внутри уровня цены?**

- Реализует FIFO: `push_back` при добавлении, `front` для чтения лучшей заявки.
- Поддерживает Time Priority без дополнительной сортировки.

**Почему `std::unordered_map<id, Order>`?**

- Поиск и удаление заявки по `id` за O(1) — необходимо для команды `CANCEL` и проверки дубликатов.

**Итоговые сложности:**

| Операция | Сложность |
|---|---|
| Добавить заявку | O(log N) |
| Удалить заявку по id | O(log N) |
| Найти заявку по id | O(1) |
| Получить лучшую BUY / SELL | O(1) |
| Итерация для PRINT | O(N) |

---

## Формат JSON-команд

Программа принимает JSON-объект с массивом `"commands"` через аргумент командной строки.

### ADD — добавить заявку

```json
{
    "type": "ADD",
    "id": 1,
    "side": "BUY",
    "price": 100,
    "quantity": 10
}
```

| Поле | Тип | Описание |
|---|---|---|
| `type` | string | `"ADD"` |
| `id` | int > 0 | Уникальный идентификатор заявки |
| `side` | string | `"BUY"` или `"SELL"` |
| `price` | int > 0 | Цена заявки |
| `quantity` | int > 0 | Количество |

### CANCEL — отменить заявку

```json
{
    "type": "CANCEL",
    "id": 1
}
```

### PRINT — вывести книгу заявок

```json
{
    "type": "PRINT"
}
```

### Полный пример входных данных

```json
{
    "commands": [
        { "type": "ADD",    "id": 1, "side": "BUY",  "price": 100, "quantity": 10 },
        { "type": "ADD",    "id": 2, "side": "SELL",  "price": 105, "quantity": 5  },
        { "type": "ADD",    "id": 3, "side": "SELL",  "price": 99,  "quantity": 7  },
        { "type": "PRINT" },
        { "type": "CANCEL", "id": 2 },
        { "type": "PRINT" }
    ]
}
```

---

## Сборка

**Требования:** CMake ≥ 3.20, компилятор с поддержкой C++17 (GCC 13+ / Clang 16+), интернет для FetchContent.

Зависимости (`nlohmann/json`, `GoogleTest`, `spdlog`) загружаются автоматически при первой сборке.

```bash
git clone <repo-url>
cd matching_engine

cmake -B build
cmake --build build
```

После сборки в директории `build` появятся:
- `matching_engine` — исполняемый файл
- `matching_engine_tests` — тестовый бинарь

---

## Запуск

```bash
./build/matching_engine '<json>'
```

JSON передаётся одним аргументом командной строки. Рекомендуется обернуть в одинарные кавычки, чтобы экранировать от shell.

**Пример:**

```bash
./build/matching_engine '{
    "commands": [
        { "type": "ADD",  "id": 1, "side": "BUY",  "price": 100, "quantity": 10 },
        { "type": "ADD",  "id": 2, "side": "SELL",  "price": 99,  "quantity": 7  },
        { "type": "PRINT" }
    ]
}'
```

---

## Запуск тестов

```bash
ctest --test-dir build
```

Или напрямую через бинарь для подробного вывода:

```bash
./build/matching_engine_tests
```

Покрытие тестами:

| Модуль | Тестов |
|---|---|
| `Order` | 14 |
| `CommandParser` | 10 |
| `OrderBook` | 16 |
| `MatchingEngine` | 16 |
| **Итого** | **57** |

---

## Примеры работы

### Сделки нет — цены не пересекаются

```bash
./build/matching_engine '{
    "commands": [
        { "type": "ADD",  "id": 1, "side": "BUY",  "price": 100, "quantity": 10 },
        { "type": "ADD",  "id": 2, "side": "SELL",  "price": 105, "quantity": 5  },
        { "type": "PRINT" }
    ]
}'
```

```
ORDER BOOK

SELL
105 5

BUY
100 10
```

---

### Полное исполнение

Обе заявки одинакового объёма — обе полностью исполняются и удаляются из книги.

```bash
./build/matching_engine '{
    "commands": [
        { "type": "ADD",  "id": 1, "side": "BUY",  "price": 100, "quantity": 10 },
        { "type": "ADD",  "id": 2, "side": "SELL",  "price": 100, "quantity": 10 },
        { "type": "PRINT" }
    ]
}'
```

```
TRADE buy=2 sell=1 price=100 quantity=10

ORDER BOOK

SELL

BUY
```

---

### Частичное исполнение

BUY на 10, SELL на 4 — BUY остаётся в книге с остатком 6.

```bash
./build/matching_engine '{
    "commands": [
        { "type": "ADD",  "id": 1, "side": "BUY",  "price": 100, "quantity": 10 },
        { "type": "ADD",  "id": 2, "side": "SELL",  "price": 100, "quantity": 4  },
        { "type": "PRINT" }
    ]
}'
```

```
TRADE buy=1 sell=2 price=100 quantity=4

ORDER BOOK

SELL

BUY
100 6
```

---

### Исполнение одной заявки против нескольких (Price Priority)

В книге три SELL-заявки по разным ценам. BUY выбирает наилучшие — сначала по цене 99, затем по 100.

```bash
./build/matching_engine '{
    "commands": [
        { "type": "ADD",  "id": 1, "side": "SELL",  "price": 101, "quantity": 5 },
        { "type": "ADD",  "id": 2, "side": "SELL",  "price": 99,  "quantity": 3 },
        { "type": "ADD",  "id": 3, "side": "SELL",  "price": 100, "quantity": 4 },
        { "type": "ADD",  "id": 4, "side": "BUY",   "price": 100, "quantity": 7 },
        { "type": "PRINT" }
    ]
}'
```

```
TRADE buy=4 sell=2 price=99 quantity=3
TRADE buy=4 sell=3 price=100 quantity=4

ORDER BOOK

SELL
101 5

BUY
```

---

### Отмена заявки

```bash
./build/matching_engine '{
    "commands": [
        { "type": "ADD",    "id": 1, "side": "SELL",  "price": 100, "quantity": 10 },
        { "type": "CANCEL", "id": 1 },
        { "type": "ADD",    "id": 2, "side": "BUY",   "price": 100, "quantity": 10 },
        { "type": "PRINT" }
    ]
}'
```

```
ORDER BOOK

SELL

BUY
100 10
```

Сделки нет — SELL была отменена до прихода BUY.

---

### MARKET Order

Для MARKET-заявок необходимо проверить:

- исполнение по лучшей доступной цене;
- исполнение одной MARKET-заявки против нескольких ценовых уровней;
- частичное исполнение при недостатке ликвидности;
- отсутствие MARKET-заявки в Order Book после обработки.

### MODIFY

Для `MODIFY` необходимо проверить:

- изменение цены;
- изменение количества;
- повторное прохождение изменённой заявки через Matching Engine;
- потерю временного приоритета при изменении цены;
- ошибку при попытке изменить несуществующую или уже исполненную заявку.

---

### Обработка ошибочной команды

Ошибочная команда не прерывает обработку; остальные команды выполняются в штатном режиме.

```bash
./build/matching_engine '{
    "commands": [
        { "type": "ADD",    "id": 1, "side": "BUY",  "price": 100, "quantity": 10 },
        { "type": "CANCEL", "id": 99 },
        { "type": "PRINT" }
    ]
}'
```

```
ERROR: Cannot cancel: order with id 99 not found

ORDER BOOK

SELL

BUY
100 10
```

---

### Логирование

Все события записываются в stdout через spdlog. Пример вывода при выполнении сделки:

```
[18:12:09] [info] Application started
[18:12:09] [info] Order received: id=1 side=SELL price=100 qty=10
[18:12:09] [info] Order added to book: id=1
[18:12:09] [info] Order received: id=2 side=BUY price=100 qty=10
[18:12:09] [info] Trade executed: buy=2 sell=1 price=100 qty=10
[18:12:09] [info] Order completely filled: id=1
[18:12:09] [info] Order completely filled: id=2
[18:12:09] [info] Order completely filled on arrival: id=2
[18:12:09] [info] Application finished
```

Логируемые события: `Application started/finished`, `Order received`, `Order added to book`, `Trade executed`, `Order partially/completely filled`, `Order cancelled`, а также все ошибки с уровнем `[error]`.

---

## Дополнительный функционал

### Market Order

Поддерживается рыночная заявка через отдельный `MarketCommand`. MARKET-заявка не содержит цены, исполняется по лучшим доступным ценам противоположной стороны и не сохраняется в Order Book после обработки.

### MODIFY

Поддерживается команда `MODIFY` через отдельный `ModifyCommand`. Команда изменяет цену и количество существующей активной заявки. После изменения заявка повторно проходит через Matching Engine; при изменении цены она теряет прежний временной приоритет.
