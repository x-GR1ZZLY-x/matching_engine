#include"pg_connection.hpp"
#include"exceptions.hpp"

namespace matching_engine{

void PgConnectionDeleter::operator()(PGconn* conn) const noexcept{
    PQfinish(conn);
}

PgConnection::PgConnection(const std::string& host, const std::string& port,
    const std::string& dbname, const std::string& user,
    const std::string& password)
    : conn_(PQsetdbLogin(host.c_str(), port.c_str(), nullptr, nullptr,
        dbname.c_str(), user.c_str(), password.c_str())){

    if(PQstatus(conn_.get()) != CONNECTION_OK){
        std::string message = PQerrorMessage(conn_.get());
        throw DatabaseError("Failed to connect to database: " + message);
    }
}

PgResult PgConnection::execute(const std::string& sql,
    const std::vector<std::optional<std::string>>& params){

    if(!conn_){
        throw DatabaseError("PgConnection::execute called without an active connection "
            "(the connection was moved from)");
    }

    std::vector<const char*> values;
    values.reserve(params.size());
    for(const auto& param : params){
        values.push_back(param.has_value() ? param->c_str() : nullptr);
    }

    PGresult* result = PQexecParams(
        conn_.get(),
        sql.c_str(),
        static_cast<int>(values.size()),
        nullptr,
        values.data(),
        nullptr,
        nullptr,
        0);

    return PgResult(result);
}

}
