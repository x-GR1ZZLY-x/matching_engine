#include"cli_args.hpp"

namespace matching_engine{

namespace{
constexpr const char* kDefaultConfigPath = "config/config.json";
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
    return true;
}

}
