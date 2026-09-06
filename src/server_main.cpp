#include<string>
#include"cli_args.hpp"
#include"config.hpp"
#include"exceptions.hpp"
#include"logger.hpp"

// Скелет серверного бинарника (REQ-NET-12): разбирает "--config", читает
// конфигурацию нового формата и завершает работу. Сетевой цикл, приём
// соединений и сигнальный поток появляются в последующих задачах.
//
// В серверном бинарнике std::cout не используется вообще: результат работы
// уходит клиенту в сокет, поэтому оба стандартных потока отданы под
// диагностику — вся она идёт через Logger, который пишет в stderr, а
// systemd соберёт оба стандартных потока в journal.
int main(int argc, char** argv){
    using namespace matching_engine;

    std::string configPath;
    std::string errorMessage;
    if(!parseServerArgs(argc, argv, configPath, errorMessage)){
        Logger::instance().error(errorMessage);
        return 1;
    }

    AppConfig config;
    try{
        config = loadConfig(configPath);
    } catch(const MatchingEngineError& e){
        Logger::instance().error(std::string("Config error: ") + e.what());
        return 1;
    }

    Logger::instance().info("Server config: address=" + config.server.address +
        " port=" + std::to_string(config.server.port) +
        " max_message_size=" + std::to_string(config.server.maxMessageSize));

    return 0;
}
