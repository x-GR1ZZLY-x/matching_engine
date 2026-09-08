#include<chrono>
#include<iostream>
#include<string>

#include"cli_args.hpp"
#include"client.hpp"
#include"exceptions.hpp"
#include"report_printer.hpp"

// Консольная программа matching-engine-client (задача 06, REQ-CLI-01..04):
// тонкая оболочка над Client (задача 04) — подключается один раз, читает
// команды со стандартного ввода построчно и переиспользует то же
// соединение до конца ввода. Своей работы с сокетами, кадрами или порядком
// байт здесь нет: она уже локализована в MessageCodec и Client.
//
// Формат ввода — одна JSON-команда на строку, ровно в том виде, в каком она
// уходит в кадре (docs/task4/02-network-protocol.md, раздел 2): строка
// отправляется как есть, без разбора и повторной пересборки JSON на стороне
// клиента — если она невалидна, об этом сообщит сервер тем же ответом
// {"status":"ERROR","error":"INVALID_REQUEST",...}, что и по сети.
//
// std::cout здесь уместен: клиент — консольная программа, и вывод ответа
// сервера — её прямое назначение (в отличие от matching-engine-server, где
// весь вывод уходит в сокет и в journal). Единственное место печати
// результата — ReportPrinter::printResponse; диагностика подключения и
// разрывов идёт туда же, через ReportPrinter::printError.
namespace {

constexpr const char* kUsage =
    "Usage: matching-engine-client --host <address> --port <port>\n";

// Совпадает с задокументированным умолчанием server.max_message_size
// (docs/task4/02-network-protocol.md, раздел 1). Клиент не читает файл
// конфигурации сервера — у него нет способа узнать настоящий предел заранее,
// поэтому берётся тот же предел, что документирован как значение по
// умолчанию. Ответ, который в этот предел не уместится, сервер сам
// сообщит кодом RESPONSE_TOO_LARGE (раздел 4.1) — соединение при этом не
// закрывается, программа продолжает работать.
constexpr std::size_t kDefaultMaxMessageSize = 1024 * 1024;

}

int main(int argc, char** argv){
    using namespace matching_engine;

    ReportPrinter printer;

    std::string host;
    std::string port;
    std::string errorMessage;
    if(!parseClientArgs(argc, argv, host, port, errorMessage)){
        printer.printError(errorMessage);
        printer.printUsage(kUsage);
        return 1;
    }

    // parseClientArgs уже проверил, что port — число в диапазоне TCP-портов
    // (0..65535), поэтому здесь разбор не может ни бросить исключение, ни
    // переполниться.
    const unsigned short portNumber = static_cast<unsigned short>(std::stoi(port));

    // Умолчание конструктора Client (2 секунды) введено ради гарантированного
    // падения сетевых тестов — для интерактивной программы оно слишком
    // короткое: ADD/CANCEL/MODIFY идут через транзакцию в PostgreSQL, и на
    // холодной или нагруженной базе коммит может занять больше двух секунд.
    // При таком умолчании клиент печатал бы "сервер не ответил вовремя" и
    // выходил бы с кодом 1, хотя заявка уже выполнена и записана.
    constexpr std::chrono::seconds kResponseTimeout(30);

    Client client(kDefaultMaxMessageSize, kResponseTimeout);
    try{
        client.connect(host, portNumber);
    } catch(const NetworkError& e){
        // Собственный префикс с адресом здесь не добавляется: Client::connect
        // уже называет хост и порт в тексте своей ошибки, и второй такой же
        // префикс превращал бы сообщение в "Failed to connect to X: Failed to
        // connect to X: Connection refused".
        printer.printError(e.what());
        return 1;
    }

    std::string line;
    while(std::getline(std::cin, line)){
        if(line.empty()){
            // Пустая строка — не команда: кадр нулевой длины сервер отверг
            // бы как невалидный JSON. Контракт это допускает, но для
            // интерактивного ввода уместнее молча пропустить строку, чем
            // тратить на неё round-trip ради предсказуемой ошибки.
            continue;
        }

        std::string frame;
        try{
            frame = client.encodeFrame(line);
        } catch(const MatchingEngineError& e){
            // Строка целиком не ушла в сокет — позиция в потоке байт не
            // испорчена, соединение остаётся рабочим (в отличие от сбоя
            // ниже, на приёме ответа). Сообщаем о конкретной строке и ждём
            // следующую, вместо того чтобы обрывать весь сеанс.
            printer.printError(std::string("Input line is too long to send: ") + e.what());
            continue;
        }

        try{
            client.sendRawBytes(frame);
            const std::string response = client.receiveFrame();
            printer.printResponse(response);
        } catch(const NetworkTimeoutError& e){
            printer.printError(std::string("Server did not respond in time: ") + e.what());
            return 1;
        } catch(const NetworkError& e){
            printer.printError(std::string("Connection to server was lost: ") + e.what());
            return 1;
        } catch(const MatchingEngineError& e){
            // receiveFrame() бросает MessageTooLargeError — не NetworkError —
            // если заголовок, присланный сервером, превышает
            // kDefaultMaxMessageSize. Позицию в потоке байт после этого
            // больше нельзя доверять, поэтому это обрабатывается так же, как
            // потеря соединения, а не продолжением работы с риском
            // разобрать следующий кадр неверно.
            printer.printError(std::string("Protocol error: ") + e.what());
            return 1;
        }
    }

    return 0;
}
