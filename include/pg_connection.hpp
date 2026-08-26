#pragma once

#include<libpq-fe.h>
#include<memory>
#include<optional>
#include<string>
#include<vector>
#include"pg_result.hpp"

namespace matching_engine{

struct PgConnectionDeleter{
    void operator()(PGconn* conn) const noexcept;
};

class PgConnection{
public:
    PgConnection(const std::string& host, const std::string& port,
        const std::string& dbname, const std::string& user,
        const std::string& password);

    PgConnection(const PgConnection&) = delete;
    PgConnection& operator=(const PgConnection&) = delete;
    PgConnection(PgConnection&&) noexcept = default;
    PgConnection& operator=(PgConnection&&) noexcept = default;

    PgResult execute(const std::string& sql,
        const std::vector<std::optional<std::string>>& params = {});

    // Выполняет sql через PQexec: в отличие от execute()/PQexecParams,
    // принимает несколько ';'-разделённых операторов в одной строке и
    // не принимает параметров. Операторы одного вызова выполняются одной
    // неявной транзакцией сервера.
    // ТОЛЬКО для доверенных файлов схемы — метод не экранирует и не
    // параметризует значения, поэтому передавать сюда пользовательский
    // ввод или данные нельзя ни при каких обстоятельствах.
    PgResult executeScript(const std::string& sql);

private:
    std::unique_ptr<PGconn, PgConnectionDeleter> conn_;
};

}
