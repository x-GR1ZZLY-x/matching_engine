#pragma once

namespace matching_engine{

// Единственный на процесс генератор номеров последовательности заявок —
// по образцу Logger::instance(). Многопоточности в проекте нет, поэтому
// синхронизация не нужна.
class SequenceGenerator {
public:
    static SequenceGenerator& instance();

    // Возвращает текущее значение счётчика и увеличивает его на единицу.
    long long next();

    // Выставляет счётчик так, что следующий next() вернёт value.
    // Нужен при восстановлении книги после рестарта (задача 08).
    void reset(long long value);

    SequenceGenerator(const SequenceGenerator&) = delete;
    SequenceGenerator& operator=(const SequenceGenerator&) = delete;
    SequenceGenerator(SequenceGenerator&&) = delete;
    SequenceGenerator& operator=(SequenceGenerator&&) = delete;

private:
    SequenceGenerator();

    long long next_;
};

}
