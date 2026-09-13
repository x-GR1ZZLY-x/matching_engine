#pragma once

#include<memory>
#include<string>

// Предварительное объявление вместо #include<libpq-fe.h>: заголовок обёртки
// не должен делать тип результата libpq видимым во всех единицах трансляции,
// которые его подключают (типы драйвера не должны
// расходиться дальше обёртки). Используется сам тег структуры (pg_result),
// а не typedef-имя PGresult, поэтому повторять
// "typedef struct pg_result PGresult;" из libpq-fe.h не нужно — оно придёт
// вместе с этим заголовком там, где действительно нужно, то есть в .cpp.
struct pg_result;

namespace matching_engine{

struct PgResultDeleter{
    void operator()(pg_result* result) const noexcept;
};

class PgResult{
public:
    explicit PgResult(pg_result* result);

    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;
    PgResult(PgResult&&) noexcept = default;
    PgResult& operator=(PgResult&&) noexcept = default;

    int rowCount() const noexcept;
    int columnCount() const noexcept;
    bool isNull(int row, int column) const;
    std::string getValue(int row, int column) const;

private:
    std::unique_ptr<pg_result, PgResultDeleter> result_;
};

}
