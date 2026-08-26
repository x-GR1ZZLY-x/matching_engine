#include<spdlog/spdlog.h>
#include<spdlog/sinks/stdout_color_sinks.h>
#include<spdlog/cfg/env.h>
#include"logger.hpp"

namespace matching_engine{

Logger::Logger(){
    auto console = spdlog::stderr_color_mt("matching_engine");
    console->set_pattern("[%H:%M:%S] [%^%l%$] %v");
    spdlog::set_default_logger(console);
    // По умолчанию отладочные записи (debug()) не печатаются в терминал —
    // это используется, чтобы прятать сырой текст драйвера БД из обычного
    // вывода. Уровень можно явно повысить штатной переменной окружения
    // spdlog SPDLOG_LEVEL (например SPDLOG_LEVEL=debug), без своих ключей
    // или переменных: load_env_levels() переопределяет уровень, если
    // переменная задана, иначе оставляет уровень info по умолчанию.
    spdlog::set_level(spdlog::level::info);
    spdlog::cfg::load_env_levels();
}

Logger& Logger::instance(){
    static Logger logger;
    return logger;
}

void Logger::info(const std::string& message){
    spdlog::info(message);
}

void Logger::warning(const std::string& message){
    spdlog::warn(message);
}

void Logger::error(const std::string& message){
    spdlog::error(message);
}

void Logger::debug(const std::string& message){
    spdlog::debug(message);
}

}