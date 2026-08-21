#include<spdlog/spdlog.h>
#include<spdlog/sinks/stdout_color_sinks.h>
#include"logger.hpp"

namespace matching_engine{

Logger::Logger(){
    auto console = spdlog::stderr_color_mt("matching_engine");
    console->set_pattern("[%H:%M:%S] [%^%l%$] %v");
    spdlog::set_default_logger(console);
    spdlog::set_level(spdlog::level::debug);
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

}