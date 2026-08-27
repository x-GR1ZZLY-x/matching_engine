#include"pg_result.hpp"
#include"exceptions.hpp"
#include<libpq-fe.h>

namespace matching_engine{

void PgResultDeleter::operator()(PGresult* result) const noexcept{
    PQclear(result);
}

PgResult::PgResult(PGresult* result) : result_(result){
    if(!result_){
        throw DatabaseError("libpq returned no result");
    }

    ExecStatusType status = PQresultStatus(result_.get());
    if(status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK){
        std::string message = PQresultErrorMessage(result_.get());
        throw DatabaseError("Query failed: " + message);
    }
}

int PgResult::rowCount() const noexcept{
    return PQntuples(result_.get());
}

int PgResult::columnCount() const noexcept{
    return PQnfields(result_.get());
}

bool PgResult::isNull(int row, int column) const{
    return PQgetisnull(result_.get(), row, column) != 0;
}

std::string PgResult::getValue(int row, int column) const{
    const char* value = PQgetvalue(result_.get(), row, column);
    return value ? std::string(value) : std::string();
}

}
