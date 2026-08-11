#pragma once

#include<string>

namespace matching_engine{
    
class Logger {
public:
    static Logger& instance();

    void info(const std::string& messaage);
    void warning(const std::string& message);
    void error(const std::string& message);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

private:
    Logger();
};

}