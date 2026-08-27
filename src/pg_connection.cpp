#include"pg_connection.hpp"
#include"exceptions.hpp"
#include"logger.hpp"
#include<libpq-fe.h>

namespace matching_engine{

namespace{

// Отводит вывод драйвером NOTICE- и WARNING-сообщений сервера (например,
// "relation ... already exists, skipping" при идемпотентном повторном
// применении схемы) из stderr процесса в Logger::debug — иначе они видны
// при каждом обычном запуске в обход ReportPrinter/Logger. На отладочном
// уровне логирования (SPDLOG_LEVEL=debug) сообщения по-прежнему видны.
void discardNotice(void*, const char* message) noexcept{
    Logger::instance().debug(message ? std::string(message) : std::string());
}

}

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

    PQsetNoticeProcessor(conn_.get(), discardNotice, nullptr);
}

PgConnection::PgConnection(PgConnection&& other)
    : conn_(nullptr){
    if(other.activeTransactionCount_ != 0){
        throw DatabaseError("Cannot move a PgConnection while a PgTransaction on it "
            "is still open");
    }
    conn_ = std::move(other.conn_);
}

PgConnection& PgConnection::operator=(PgConnection&& other){
    if(this == &other){
        return *this;
    }
    if(activeTransactionCount_ != 0 || other.activeTransactionCount_ != 0){
        throw DatabaseError("Cannot move a PgConnection while a PgTransaction on it "
            "is still open");
    }
    conn_ = std::move(other.conn_);
    return *this;
}

PgConnection::~PgConnection(){
    if(activeTransactionCount_ != 0){
        Logger::instance().error("PgConnection destroyed while " +
            std::to_string(activeTransactionCount_) +
            " PgTransaction(s) on it are still open");
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

PgResult PgConnection::executeScript(const std::string& sql){
    if(!conn_){
        throw DatabaseError("PgConnection::executeScript called without an active connection "
            "(the connection was moved from)");
    }

    return PgResult(PQexec(conn_.get(), sql.c_str()));
}

}
