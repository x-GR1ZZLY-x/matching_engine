#pragma once

#include<libpq-fe.h>
#include<memory>
#include<string>

namespace matching_engine{

struct PgResultDeleter{
    void operator()(PGresult* result) const noexcept;
};

class PgResult{
public:
    explicit PgResult(PGresult* result);

    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;
    PgResult(PgResult&&) noexcept = default;
    PgResult& operator=(PgResult&&) noexcept = default;

    int rowCount() const noexcept;
    int columnCount() const noexcept;
    bool isNull(int row, int column) const;
    std::string getValue(int row, int column) const;

private:
    std::unique_ptr<PGresult, PgResultDeleter> result_;
};

}
