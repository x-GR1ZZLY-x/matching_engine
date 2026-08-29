#include<gtest/gtest.h>
#include<string>
#include<vector>
#include"application.hpp"

using namespace matching_engine;

namespace {

// Помощник: собирает argv из строк и вызывает Application::parseArgs.
bool callParseArgs(std::vector<std::string> args, std::string& configPath,
    std::string& jsonArg, std::string& replayPath, std::string& errorMessage){

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("matching_engine"));
    for(auto& arg : args){
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    return Application::parseArgs(static_cast<int>(argv.size()), argv.data(),
        configPath, jsonArg, replayPath, errorMessage);
}

}

TEST(ApplicationParseArgsTest, DefaultConfigPathWhenNoConfigOption){
    std::string configPath, jsonArg, replayPath, error;

    const bool ok = callParseArgs({"{\"commands\":[]}"}, configPath, jsonArg, replayPath, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "config/database.json");
    EXPECT_EQ(jsonArg, "{\"commands\":[]}");
    EXPECT_TRUE(replayPath.empty());
}

TEST(ApplicationParseArgsTest, ConfigOptionBeforePositionalArg){
    std::string configPath, jsonArg, replayPath, error;

    const bool ok = callParseArgs({"--config", "custom.json", "{\"commands\":[]}"},
        configPath, jsonArg, replayPath, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "custom.json");
    EXPECT_EQ(jsonArg, "{\"commands\":[]}");
}

TEST(ApplicationParseArgsTest, ConfigOptionAfterPositionalArg){
    std::string configPath, jsonArg, replayPath, error;

    const bool ok = callParseArgs({"{\"commands\":[]}", "--config", "custom.json"},
        configPath, jsonArg, replayPath, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "custom.json");
    EXPECT_EQ(jsonArg, "{\"commands\":[]}");
}

TEST(ApplicationParseArgsTest, ConfigOptionWithoutValueFails){
    std::string configPath, jsonArg, replayPath, error;

    const bool ok = callParseArgs({"--config"}, configPath, jsonArg, replayPath, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

// --replay появился в задаче 10; до неё этот же вызов использовался как
// заглушка для проверки "неизвестный ключ отвергается" (см.
// docs/progress.md). Теперь --replay — известный ключ с собственными
// тестами ниже, а неизвестный ключ проверяется на другом имени.
TEST(ApplicationParseArgsTest, UnknownOptionFails){
    std::string configPath, jsonArg, replayPath, error;

    const bool ok = callParseArgs({"--verbose"}, configPath, jsonArg, replayPath, error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "Unknown option: --verbose");
}

TEST(ApplicationParseArgsTest, ExtraPositionalArgFails){
    std::string configPath, jsonArg, replayPath, error;

    const bool ok = callParseArgs({"{\"commands\":[]}", "{\"commands\":[]}"},
        configPath, jsonArg, replayPath, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

TEST(ApplicationParseArgsTest, ReplayOptionWithPathSucceeds){
    std::string configPath, jsonArg, replayPath, error;

    const bool ok = callParseArgs({"--replay", "workload.jsonl"},
        configPath, jsonArg, replayPath, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(replayPath, "workload.jsonl");
    EXPECT_TRUE(jsonArg.empty());
}

TEST(ApplicationParseArgsTest, ReplayOptionWithoutValueFails){
    std::string configPath, jsonArg, replayPath, error;

    const bool ok = callParseArgs({"--replay"}, configPath, jsonArg, replayPath, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

TEST(ApplicationParseArgsTest, ReplayTogetherWithPositionalJsonFails){
    std::string configPath, jsonArg, replayPath, error;

    const bool ok = callParseArgs({"--replay", "workload.jsonl", "{\"commands\":[]}"},
        configPath, jsonArg, replayPath, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

TEST(ApplicationParseArgsTest, ReplayTogetherWithConfigOptionParsesBoth){
    std::string configPath, jsonArg, replayPath, error;

    const bool ok = callParseArgs({"--config", "custom.json", "--replay", "workload.jsonl"},
        configPath, jsonArg, replayPath, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "custom.json");
    EXPECT_EQ(replayPath, "workload.jsonl");
}
