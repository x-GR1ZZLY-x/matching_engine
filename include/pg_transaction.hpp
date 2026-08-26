#pragma once

#include"pg_connection.hpp"

namespace matching_engine{

class PgTransaction{
public:
    explicit PgTransaction(PgConnection& connection);

    PgTransaction(const PgTransaction&) = delete;
    PgTransaction& operator=(const PgTransaction&) = delete;
    PgTransaction(PgTransaction&&) = delete;
    PgTransaction& operator=(PgTransaction&&) = delete;

    ~PgTransaction();

    void commit();

private:
    PgConnection& connection_;
    bool committed_;
};

}
