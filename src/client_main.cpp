#include<iostream>
#include<string>
#include"cli_args.hpp"

// Скелет клиентского бинарника (REQ-NET-12): разбирает "--host"/"--port" и
// сообщает о них. Подключение к серверу появляется позже вместе с классом
// Client — сейчас соединяться ещё некуда.
//
// В отличие от серверного бинарника, std::cout здесь уместен: вывод
// результата — прямое назначение клиента.
int main(int argc, char** argv){
    using namespace matching_engine;

    std::string host;
    std::string port;
    std::string errorMessage;
    if(!parseClientArgs(argc, argv, host, port, errorMessage)){
        std::cerr << "ERROR: " << errorMessage << "\n";
        return 1;
    }

    std::cout << "Client configured for " << host << ":" << port << "\n";
    return 0;
}
