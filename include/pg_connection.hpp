#pragma once

#include<memory>
#include<optional>
#include<string>
#include<vector>
#include"pg_result.hpp"

// Предварительное объявление вместо #include<libpq-fe.h>: используется сам
// тег структуры (pg_conn), а не typedef-имя PGconn, поэтому повторять
// "typedef struct pg_conn PGconn;" из libpq-fe.h не нужно — оно придёт
// вместе с этим заголовком там, где действительно нужно, то есть в .cpp.
struct pg_conn;

namespace matching_engine{

struct PgConnectionDeleter{
    void operator()(pg_conn* conn) const noexcept;
};

class PgConnection{
public:
    PgConnection(const std::string& host, const std::string& port,
        const std::string& dbname, const std::string& user,
        const std::string& password);

    PgConnection(const PgConnection&) = delete;
    PgConnection& operator=(const PgConnection&) = delete;

    // Не noexcept и не = default: перемещение соединения с живой
    // (не разрушенной) PgTransaction опустошило бы conn_ под её ссылкой —
    // ROLLBACK в деструкторе транзакции обнаружил бы пустое соединение,
    // бросил бы, catch(...) проглотил бы, и транзакция осталась бы висеть
    // на сервере (критерий 9 задачи 06). Поэтому обе операции проверяют
    // счётчик активных транзакций и бросают DatabaseError, если он не нулевой.
    PgConnection(PgConnection&& other);
    PgConnection& operator=(PgConnection&& other);

    // Явный деструктор (а не = default): если соединение уничтожается при
    // ненулевом счётчике активных транзакций, значит где-то ещё жива
    // PgTransaction со ссылкой на это соединение — её деструктор вызовет
    // ROLLBACK на уже разрушенном объекте (неопределённое поведение).
    // Сегодня в коде такой сценарий не встречается, но раз счётчик уже
    // ведётся, дыра закрывается диагностикой в лог; бросать из деструктора
    // нельзя, поэтому только запись уровня error.
    ~PgConnection();

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
    // Только PgTransaction ведёт счёт живых транзакций на этом соединении.
    friend class PgTransaction;
    void addActiveTransaction() noexcept{ ++activeTransactionCount_; }
    void removeActiveTransaction() noexcept{ --activeTransactionCount_; }

    std::unique_ptr<pg_conn, PgConnectionDeleter> conn_;
    int activeTransactionCount_ = 0;
};

}
