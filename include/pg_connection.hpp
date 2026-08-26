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

private:
    std::unique_ptr<PGconn, PgConnectionDeleter> conn_;
};

}
