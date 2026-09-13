#include<gtest/gtest.h>
#include<string>
#include<vector>
#include"cli_args.hpp"

using namespace matching_engine;

namespace {

// Помощники: собирают argv из строк и вызывают parseServerArgs/parseClientArgs.
bool callParseServerArgs(std::vector<std::string> args, std::string& configPath,
    std::string& errorMessage){

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("matching-engine-server"));
    for(auto& arg : args){
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    return parseServerArgs(static_cast<int>(argv.size()), argv.data(), configPath,
        errorMessage);
}

bool callParseClientArgs(std::vector<std::string> args, std::string& host, std::string& port,
    std::string& errorMessage){

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("matching-engine-client"));
    for(auto& arg : args){
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    return parseClientArgs(static_cast<int>(argv.size()), argv.data(), host, port,
        errorMessage);
}

}

TEST(CliArgsTest, ServerDefaultConfigPathWhenNoConfigOption){
    std::string configPath, error;

    const bool ok = callParseServerArgs({}, configPath, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "config/config.json");
}

TEST(CliArgsTest, ServerConfigOptionWithValueSucceeds){
    std::string configPath, error;

    const bool ok = callParseServerArgs({"--config", "custom.json"}, configPath, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "custom.json");
}

TEST(CliArgsTest, ServerConfigOptionWithoutValueFails){
    std::string configPath, error;

    const bool ok = callParseServerArgs({"--config"}, configPath, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

TEST(CliArgsTest, ServerUnknownOptionFails){
    std::string configPath, error;

    const bool ok = callParseServerArgs({"--verbose"}, configPath, error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "Unknown option: --verbose");
}

TEST(CliArgsTest, ClientWithHostAndPortSucceeds){
    std::string host, port, error;

    const bool ok = callParseClientArgs({"--host", "127.0.0.1", "--port", "9000"}, host, port,
        error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(host, "127.0.0.1");
    EXPECT_EQ(port, "9000");
}

TEST(CliArgsTest, ClientWithoutPortFails){
    std::string host, port, error;

    const bool ok = callParseClientArgs({"--host", "127.0.0.1"}, host, port, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

// Нечисловой порт обязан отвергаться уже при разборе
// argv, а не приводить к std::invalid_argument из std::stoi где-то глубже.
TEST(CliArgsTest, ClientWithNonNumericPortFails){
    std::string host, port, error;

    const bool ok = callParseClientArgs({"--host", "127.0.0.1", "--port", "abc"}, host, port,
        error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

// Верхняя граница диапазона (65535) обязана приниматься: без этого теста
// замена "<= 65535" на "< 65535" в isValidPort() не роняла бы ни одного
// теста.
TEST(CliArgsTest, ClientWithMaxValidPortSucceeds){
    std::string host, port, error;

    const bool ok = callParseClientArgs({"--host", "127.0.0.1", "--port", "65535"}, host, port,
        error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(port, "65535");
}

TEST(CliArgsTest, ClientWithOutOfRangePortFails){
    std::string host, port, error;

    const bool ok = callParseClientArgs({"--host", "127.0.0.1", "--port", "99999"}, host, port,
        error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

// Отсутствие --host не покрыто ни одним существующим тестом: без него
// пропала бы ветка "host.empty()" в parseClientArgs — единственный тест на
// отсутствующий аргумент до этого проверял только --port.
TEST(CliArgsTest, ClientWithoutHostFails){
    std::string host, port, error;

    const bool ok = callParseClientArgs({"--port", "9000"}, host, port, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

// "--host" без значения — отдельная ветка от "--host отсутствует вовсе":
// здесь опция встречена, но argv кончился раньше её значения.
TEST(CliArgsTest, ClientHostOptionWithoutValueFails){
    std::string host, port, error;

    const bool ok = callParseClientArgs({"--host"}, host, port, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

// Тот же случай для "--port": опция есть, значения нет.
TEST(CliArgsTest, ClientPortOptionWithoutValueFails){
    std::string host, port, error;

    const bool ok = callParseClientArgs({"--host", "127.0.0.1", "--port"}, host, port, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

// Неизвестный флаг у клиента — ветка else в parseClientArgs, отдельная от
// той же ветки у сервера (ServerUnknownOptionFails покрывает только
// parseServerArgs).
TEST(CliArgsTest, ClientUnknownOptionFails){
    std::string host, port, error;

    const bool ok = callParseClientArgs({"--verbose"}, host, port, error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "Unknown option: --verbose");
}

// Порт длиннее пяти цифр обязан отсекаться проверкой длины в isValidPort до
// вызова std::stoi: "99999" (пять цифр, отвергается по значению) не
// упражняет ветку "port.size() > 5" отдельно от ветки диапазона значений.
TEST(CliArgsTest, ClientWithPortLongerThanFiveDigitsFails){
    std::string host, port, error;

    const bool ok = callParseClientArgs({"--host", "127.0.0.1", "--port", "123456"}, host, port,
        error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}
