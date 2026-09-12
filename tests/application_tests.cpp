#include<gtest/gtest.h>
#include<string>
#include<vector>
#include"application.hpp"

using namespace matching_engine;

namespace {

// Помощник: собирает argv из строк и вызывает Application::parseArgs.
bool callParseArgs(std::vector<std::string> args, std::string& configPath,
    std::string& jsonArg, std::string& replayPath, bool& batch, std::string& errorMessage){

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("matching_engine"));
    for(auto& arg : args){
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    return Application::parseArgs(static_cast<int>(argv.size()), argv.data(),
        configPath, jsonArg, replayPath, batch, errorMessage);
}

}

TEST(ApplicationParseArgsTest, DefaultConfigPathWhenNoConfigOption){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"{\"commands\":[]}"}, configPath, jsonArg, replayPath, batch,
        error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "config/config.json");
    EXPECT_EQ(jsonArg, "{\"commands\":[]}");
    EXPECT_TRUE(replayPath.empty());
    EXPECT_FALSE(batch);
}

TEST(ApplicationParseArgsTest, ConfigOptionBeforePositionalArg){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--config", "custom.json", "{\"commands\":[]}"},
        configPath, jsonArg, replayPath, batch, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "custom.json");
    EXPECT_EQ(jsonArg, "{\"commands\":[]}");
}

TEST(ApplicationParseArgsTest, ConfigOptionAfterPositionalArg){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"{\"commands\":[]}", "--config", "custom.json"},
        configPath, jsonArg, replayPath, batch, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "custom.json");
    EXPECT_EQ(jsonArg, "{\"commands\":[]}");
}

TEST(ApplicationParseArgsTest, ConfigOptionWithoutValueFails){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--config"}, configPath, jsonArg, replayPath, batch, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

// --replay появился позже; до этого этот же вызов использовался как
// заглушка для проверки "неизвестный ключ отвергается". Теперь --replay — известный ключ с собственными
// тестами ниже, а неизвестный ключ проверяется на другом имени.
TEST(ApplicationParseArgsTest, UnknownOptionFails){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--verbose"}, configPath, jsonArg, replayPath, batch, error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "Unknown option: --verbose");
}

TEST(ApplicationParseArgsTest, ExtraPositionalArgFails){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"{\"commands\":[]}", "{\"commands\":[]}"},
        configPath, jsonArg, replayPath, batch, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

TEST(ApplicationParseArgsTest, ReplayOptionWithPathSucceeds){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--replay", "workload.jsonl"},
        configPath, jsonArg, replayPath, batch, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(replayPath, "workload.jsonl");
    EXPECT_TRUE(jsonArg.empty());
    EXPECT_FALSE(batch);
}

TEST(ApplicationParseArgsTest, ReplayOptionWithoutValueFails){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--replay"}, configPath, jsonArg, replayPath, batch, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

TEST(ApplicationParseArgsTest, ReplayTogetherWithPositionalJsonFails){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--replay", "workload.jsonl", "{\"commands\":[]}"},
        configPath, jsonArg, replayPath, batch, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

TEST(ApplicationParseArgsTest, ReplayTogetherWithConfigOptionParsesBoth){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--config", "custom.json", "--replay", "workload.jsonl"},
        configPath, jsonArg, replayPath, batch, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "custom.json");
    EXPECT_EQ(replayPath, "workload.jsonl");
}

// --batch появился позже (пакетная запись при
// воспроизведении нагрузки) — те же три сценария, что проверялись для
// --replay: принят вместе с --replay, без значения (флаг), и в
// недопустимом сочетании (без --replay).
TEST(ApplicationParseArgsTest, BatchOptionWithReplaySucceeds){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--replay", "workload.jsonl", "--batch"},
        configPath, jsonArg, replayPath, batch, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(replayPath, "workload.jsonl");
    EXPECT_TRUE(batch);
}

TEST(ApplicationParseArgsTest, BatchOptionBeforeReplaySucceeds){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--batch", "--replay", "workload.jsonl"},
        configPath, jsonArg, replayPath, batch, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(replayPath, "workload.jsonl");
    EXPECT_TRUE(batch);
}

TEST(ApplicationParseArgsTest, BatchOptionWithConfigAndReplayParsesAll){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--config", "custom.json", "--replay", "workload.jsonl",
        "--batch"}, configPath, jsonArg, replayPath, batch, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "custom.json");
    EXPECT_EQ(replayPath, "workload.jsonl");
    EXPECT_TRUE(batch);
}

// --batch — флаг, а не опция со значением: следующий аргумент не
// поглощается и остаётся отдельным значением для своей собственной опции
// (--batch стоит не последним, иначе поглощать нечего и тест совпадает с
// BatchOptionWithReplaySucceeds).
TEST(ApplicationParseArgsTest, BatchOptionTakesNoValue){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--replay", "workload.jsonl", "--batch", "--config",
        "custom.json"}, configPath, jsonArg, replayPath, batch, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(replayPath, "workload.jsonl");
    EXPECT_EQ(configPath, "custom.json");
}

TEST(ApplicationParseArgsTest, BatchOptionWithoutReplayFails){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--batch"}, configPath, jsonArg, replayPath, batch, error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "Option --batch requires --replay");
}

TEST(ApplicationParseArgsTest, BatchOptionWithPositionalJsonFails){
    std::string configPath, jsonArg, replayPath, error;
    bool batch = false;

    const bool ok = callParseArgs({"--batch", "{\"commands\":[]}"},
        configPath, jsonArg, replayPath, batch, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}
