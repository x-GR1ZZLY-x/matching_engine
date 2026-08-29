#!/usr/bin/env python3
"""Генератор файла нагрузки для режима `matching_engine --replay`.

Создаёт файл в формате JSON Lines (по одной JSON-команде на строку) —
именно то, что читает `Application::runReplay` (docs/tasks/task-10.md).
Каждая строка — тот же JSON, что принимает `CommandParser` в обычном
режиме, обязательно с полем "command_id".

Смесь команд:
  - ADD-заявки на обеих сторонах (BUY/SELL);
  - часть ADD-заявок ставится с ценой, пересекающей противоположную
    сторону книги, — они порождают сделки (TRADE);
  - часть ADD-заявок ставится заведомо глубоко в книге (далеко от цены
    пересечения) — такие почти наверняка остаются неисполненными и
    становятся целями для CANCEL;
  - CANCEL-команды, отменяющие ранее добавленные "глубокие" заявки; одна
    и та же заявка не отменяется дважды.

MODIFY и MARKET-заявки не генерируются — задача 10 их не требует.

Идентификаторы заявок (`id`) и идентификаторы команд (`command_id`)
уникальны в пределах файла (критерий 8 задачи 10): повторное использование
`id` уже исполненной заявки перезаписало бы историческую строку в `orders`.
`command_id` дополнительно несёт зерно генератора (`cmd-s<seed>-<index>`),
иначе два файла, сгенерированных с разными зёрнами, дали бы одинаковые
`command_id` и второй прогон целиком ушёл бы в кеш идемпотентности вместо
записи в БД (ревью задачи 10, правка 2).

Замер производительности (задача 11/12): перед прогоном, который
предполагается измерять, три таблицы (`orders`, `trades`,
`processed_commands`) должны быть пусты. Прогон на непустой базе с уже
встречавшимися `command_id` уйдёт в кеш идемпотентности и измерит поиск в
хеш-таблице вместо движка и записи в БД — самый заметный симптом:
`elapsed_ms`, близкий к нулю.

Запуск (Python 3, без сторонних зависимостей):

    python3 scripts/generate_workload.py
    python3 scripts/generate_workload.py --count 200000 --output big.jsonl
    python3 scripts/generate_workload.py --count 100000 --seed 7

Аргументы:
    --count   количество команд в файле (по умолчанию 100000); код
              проверяет только --count > 0, малые значения тоже допустимы
              и полезны для быстрых проверок
    --output  путь к выходному файлу (по умолчанию workload.jsonl)
    --seed    зерно генератора случайных чисел; фиксировано по умолчанию,
              чтобы задача 12 могла сравнивать "до и после" на одном и том
              же файле
"""

import argparse
import json
import random

# Все цены живут вокруг этого центра. "Агрессивные" ADD ставятся в узком
# коридоре вокруг центра и почти всегда пересекаются с противоположной
# стороной книги, порождая сделки. "Глубокие" ADD ставятся заметно дальше
# от центра и в этот коридор попасть не могут, поэтому остаются в книге
# неисполненными и служат целями для CANCEL.
CENTER_PRICE = 10_000
AGGRESSIVE_SPREAD = 2
DEEP_MIN_OFFSET = 10
DEEP_MAX_OFFSET = 200

# Доля команд, приходящихся на отмену (остальное — ADD).
CANCEL_SHARE = 0.2
# Доля ADD-команд, ставящихся агрессивно (у противоположной стороны).
AGGRESSIVE_SHARE = 0.5
# Вероятность выбрать CANCEL на очередном шаге, если уже есть цели.
CANCEL_STEP_PROBABILITY = 0.3


def build_add_command(command_index, order_id, rng, seed):
    """Формирует одну ADD-команду и признак, является ли она "глубокой"
    (то есть подходящей целью для последующей отмены)."""

    side = "BUY" if rng.random() < 0.5 else "SELL"
    quantity = rng.randint(1, 100)
    is_aggressive = rng.random() < AGGRESSIVE_SHARE

    if is_aggressive:
        price = CENTER_PRICE + rng.randint(-AGGRESSIVE_SPREAD, AGGRESSIVE_SPREAD)
        is_deep = False
    else:
        offset = rng.randint(DEEP_MIN_OFFSET, DEEP_MAX_OFFSET)
        price = CENTER_PRICE - offset if side == "BUY" else CENTER_PRICE + offset
        is_deep = True

    command = {
        "type": "ADD",
        "id": order_id,
        "side": side,
        "price": price,
        "quantity": quantity,
        "command_id": f"cmd-s{seed}-{command_index}",
    }
    return command, is_deep


def build_cancel_command(command_index, target_order_id, seed):
    return {
        "type": "CANCEL",
        "id": target_order_id,
        "command_id": f"cmd-s{seed}-{command_index}",
    }


def generate_commands(count, rng, seed):
    """Возвращает список команд длиной ровно `count`, соблюдая доли ADD и
    CANCEL, уникальность id/command_id, и то, что CANCEL всегда целится в
    заявку, уже встреченную раньше в этом же списке."""

    cancel_target_count = int(count * CANCEL_SHARE)
    add_target_count = count - cancel_target_count

    commands = []
    pending_deep_ids = []  # "Глубокие" заявки, ещё не отменённые.
    next_order_id = 1
    added = 0
    cancelled = 0

    for command_index in range(count):
        remaining_add = add_target_count - added
        remaining_cancel = cancel_target_count - cancelled

        want_cancel = (
            remaining_cancel > 0
            and pending_deep_ids
            and (remaining_add == 0 or rng.random() < CANCEL_STEP_PROBABILITY)
        )

        if want_cancel:
            target_index = rng.randrange(len(pending_deep_ids))
            target_id = pending_deep_ids.pop(target_index)
            commands.append(build_cancel_command(command_index, target_id, seed))
            cancelled += 1
        else:
            command, is_deep = build_add_command(command_index, next_order_id, rng, seed)
            commands.append(command)
            if is_deep:
                pending_deep_ids.append(next_order_id)
            next_order_id += 1
            added += 1

    return commands


def main():
    parser = argparse.ArgumentParser(
        description="Генератор файла нагрузки для matching_engine --replay")
    parser.add_argument("--count", type=int, default=100_000,
        help="количество команд в файле (по умолчанию 100000)")
    parser.add_argument("--output", type=str, default="workload.jsonl",
        help="путь к выходному файлу (по умолчанию workload.jsonl)")
    parser.add_argument("--seed", type=int, default=42,
        help="зерно генератора случайных чисел (по умолчанию 42)")
    args = parser.parse_args()

    if args.count <= 0:
        raise SystemExit("--count must be positive")

    rng = random.Random(args.seed)
    commands = generate_commands(args.count, rng, args.seed)

    with open(args.output, "w", encoding="utf-8") as out:
        for command in commands:
            out.write(json.dumps(command))
            out.write("\n")

    print(f"Wrote {len(commands)} commands to {args.output}")


if __name__ == "__main__":
    main()
