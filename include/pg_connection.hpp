#pragma once

#include<memory>
#include<optional>
#include<set>
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

    // Готовит sql под именем name один раз за сессию (PQprepare) и на всех
    // следующих вызовах с тем же именем сразу выполняет его (PQexecPrepared),
    // экономя разбор и планирование запроса. Значения передаются отдельным
    // массивом, как и в execute() — конкатенации в текст запроса нет.
    //
    // Подготовленный запрос живёт до конца сессии и откатом транзакции не
    // отменяется — проверено на используемом сервере: запрос, подготовленный
    // внутри транзакции, остаётся в pg_prepared_statements после ROLLBACK и
    // продолжает выполняться. Поэтому имени, однажды попавшего в
    // preparedStatements_, достаточно, и повторной подготовки не требуется.
    PgResult executePrepared(const std::string& name, const std::string& sql,
        const std::vector<std::optional<std::string>>& params = {});

    // Выполняет sql через PQexec: в отличие от execute()/PQexecParams,
    // принимает несколько ';'-разделённых операторов в одной строке и
    // не принимает параметров. Операторы одного вызова выполняются одной
    // неявной транзакцией сервера.
    // ТОЛЬКО для доверенных файлов схемы — метод не экранирует и не
    // параметризует значения, поэтому передавать сюда пользовательский
    // ввод или данные нельзя ни при каких обстоятельствах.
    PgResult executeScript(const std::string& sql);

    // PQstatus == CONNECTION_OK — отражает состояние, известное libpq по
    // итогам последней выполненной команды, а не результат опроса сервера
    // прямо сейчас: PQstatus не обращается к сети, только читает поле уже
    // установленного соединения. Молчаливо оборванное соединение (сервер
    // умер, а команд с тех пор не было) обнаруживается только на следующем
    // запросе — до этого isConnected() честно, но устаревше вернёт true.
    // Единственный потребитель сегодня — RequestRouter, вычисляющий поле
    // "database" ответа HEALTH (docs/task4/02-network-protocol.md, раздел
    // 3.3): значение обязано отражать последнее известное состояние, а не
    // быть литералом. noexcept: тело не бросает ничего, читая уже готовое
    // поле.
    bool isConnected() const noexcept;

private:
    // Только PgTransaction ведёт счёт живых транзакций на этом соединении.
    friend class PgTransaction;
    void addActiveTransaction() noexcept{ ++activeTransactionCount_; }
    void removeActiveTransaction() noexcept{ --activeTransactionCount_; }

    std::unique_ptr<pg_conn, PgConnectionDeleter> conn_;
    int activeTransactionCount_ = 0;

    // Имена запросов, уже подготовленных (PQprepare) на текущей сессии этого
    // соединения. Живёт в сессии, а не в объекте языка C++ — поэтому обязано
    // переезжать вместе с conn_ при перемещении PgConnection, иначе после
    // move этот набор считал бы подготовленными запросы, которых на самом
    // деле подготовило другое, уже перемещённое соединение.
    std::set<std::string> preparedStatements_;
};

}
