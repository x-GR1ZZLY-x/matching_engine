#pragma once
#include<string>
#include"command_parser.hpp"
#include"command_processor.hpp"
#include"pg_connection.hpp"
#include"report_printer.hpp"

namespace matching_engine{

class Application{
public:
    int run(int argc, char** argv);

    // Разбирает argv: возвращает путь к конфигу (--config или значение по
    // умолчанию), JSON-пакет команд (обычный режим) либо путь к файлу
    // нагрузки (--replay, режим воспроизведения). Ровно один из jsonArg /
    // replayPath заполнен при успешном разборе — режимы взаимоисключающие.
    // batch (--batch, задача 12) допустим только вместе с --replay — без
    // него это ошибка разбора, как и прочие недопустимые сочетания
    // (docs/tasks/task-12.md, критерий 4).
    // Возвращает false, если обработку нужно прервать — тогда errorMessage
    // всегда содержит текст для вывода (справку по использованию либо
    // описание конкретной ошибки разбора).
    static bool parseArgs(int argc, char** argv, std::string& configPath,
        std::string& jsonArg, std::string& replayPath, bool& batch, std::string& errorMessage);

private:
    CommandParser parser_;
    CommandProcessor processor_;
    ReportPrinter printer_;

    // Исход обработки одной команды — чем закончился вызов processCommand,
    // без пересчёта его через число сделок (сентинел вроде -1 не различал
    // бы "сделок не было", "PRINT" и "обслужено из кеша идемпотентности").
    // Applied — команда сопоставлена и записана в БД в этом вызове; trades
    // содержит число сделок именно этой записи. Duplicate — command_id уже
    // был в кеше CommandProcessor, ничего не записано, trades == 0 (число
    // сделок исходной записи здесь не нужно — см. правку 1 ревью задачи
    // 10). Printed — команда PRINT, книга напечатана. Failed — ошибка
    // разбора или доменная ошибка, команда пропущена (PersistenceError
    // сюда не относится — она не перехватывается этим методом и улетает
    // наверх, как и раньше).
    struct CommandOutcome{
        enum class Status{ Applied, Duplicate, Printed, Failed };
        Status status = Status::Failed;
        int trades = 0;
    };

    // connection передаётся параметром, а не хранится полем Application —
    // соединение живёт в run() (см. её тело) и передаётся по ссылке на
    // каждый вызов, как и в репозиториях/CommandProcessor.
    //
    // printTrades различает обычный режим (true — каждая сделка печатается
    // сразу, поведение не изменилось) и replay (false — сделки только
    // считаются, см. runReplay).
    //
    // batch — только для режима --replay --batch (задача 12): сопоставление
    // и запись в кеш идемпотентности идут как обычно (CommandProcessor::
    // processBatched), но запись в БД откладывается до flushBatch и connection
    // в этой ветке не используется. В штатном режиме batch всегда false, и
    // вызывается CommandProcessor::process — без единого изменения поведения.
    CommandOutcome processCommand(const nlohmann::json& commandJson, PgConnection& connection,
        bool printTrades, bool batch);

    // Режим воспроизведения нагрузки (задача 10, REQ-PERF-02): читает файл
    // построчно (по одной JSON-команде на строку), прогоняет каждую строку
    // через ту же processCommand/CommandProcessor/PersistenceService, что и
    // обычный режим, и печатает итоговую сводку через ReportPrinter.
    // PersistenceError фатальна и здесь не перехватывается — распространяется
    // в run(), который уже ловит её вокруг вызова. Возвращает false, если
    // файл не удалось открыть, либо если чтение оборвалось ошибкой потока
    // (ревью задачи 10, правка 4 — например путь указывает на каталог:
    // std::ifstream открывается успешно, но getline проваливается на первом
    // же вызове) — в обоих случаях run() должен завершиться с кодом 1.
    // Иначе true — даже если часть строк файла была пропущена как ошибочная
    // (это не ошибка потока, а ожидаемый разбор построчно).
    //
    // batch включает пакетную запись (задача 12, REQ-OPT-03): вместо
    // транзакции на команду накопленное пишется одной транзакцией каждые
    // kReplayBatchSize команд и остатком после конца файла. Только для
    // --replay — штатный режим (Application::run без --replay) этот
    // параметр не передаёт вовсе.
    bool runReplay(const std::string& path, PgConnection& connection, bool batch);
};

}