#include"cli_args.hpp"

#include<cctype>

namespace matching_engine{

namespace{
constexpr const char* kDefaultConfigPath = "config/config.json";

// Порт из argv приходит строкой: разбор аргументов не знает, дойдёт ли
// значение до TCP-стека вовсе (Client::connect принимает unsigned short), а
// требуется внятная ошибка уже здесь, при разборе
// argv, а не там, где стоковый std::stoi без проверки бросил бы
// невыловленное исключение или молча обрезал бы "9000abc" до 9000.
bool isValidPort(const std::string& port){
    if(port.empty() || port.size() > 5){
        return false;
    }
    for(char c : port){
        if(!std::isdigit(static_cast<unsigned char>(c))){
            return false;
        }
    }
    // Здесь port.size() <= 5, поэтому std::stoi не переполнит int.
    const int value = std::stoi(port);
    return value >= 0 && value <= 65535;
}
}

bool parseServerArgs(int argc, char** argv, std::string& configPath,
    std::string& errorMessage){
    configPath = kDefaultConfigPath;
    errorMessage.clear();

    for(int i = 1; i < argc; ++i){
        const std::string arg = argv[i];
        if(arg == "--config"){
            if(i + 1 >= argc){
                errorMessage = "Option --config requires a path argument";
                return false;
            }
            configPath = argv[++i];
        } else {
            errorMessage = "Unknown option: " + arg;
            return false;
        }
    }
    return true;
}

bool parseClientArgs(int argc, char** argv, std::string& host, std::string& port,
    std::string& errorMessage){
    host.clear();
    port.clear();
    errorMessage.clear();

    for(int i = 1; i < argc; ++i){
        const std::string arg = argv[i];
        if(arg == "--host"){
            if(i + 1 >= argc){
                errorMessage = "Option --host requires a value";
                return false;
            }
            host = argv[++i];
        } else if(arg == "--port"){
            if(i + 1 >= argc){
                errorMessage = "Option --port requires a value";
                return false;
            }
            port = argv[++i];
        } else {
            errorMessage = "Unknown option: " + arg;
            return false;
        }
    }

    if(host.empty()){
        errorMessage = "Option --host is required";
        return false;
    }
    if(port.empty()){
        errorMessage = "Option --port is required";
        return false;
    }
    if(!isValidPort(port)){
        errorMessage = "Option --port must be a number between 0 and 65535, got: " + port;
        return false;
    }
    return true;
}

}
