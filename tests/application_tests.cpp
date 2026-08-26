#include<gtest/gtest.h>
#include<string>
#include<vector>
#include"application.hpp"

using namespace matching_engine;

namespace {

// Помощник: собирает argv из строк и вызывает Application::parseArgs.
bool callParseArgs(std::vector<std::string> args, std::string& configPath,
    std::string& jsonArg, std::string& errorMessage){

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("matching_engine"));
    for(auto& arg : args){
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    return Application::parseArgs(static_cast<int>(argv.size()), argv.data(),
        configPath, jsonArg, errorMessage);
}

}

TEST(ApplicationParseArgsTest, DefaultConfigPathWhenNoConfigOption){
    std::string configPath, jsonArg, error;

    const bool ok = callParseArgs({"{\"commands\":[]}"}, configPath, jsonArg, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "config/database.json");
    EXPECT_EQ(jsonArg, "{\"commands\":[]}");
}

TEST(ApplicationParseArgsTest, ConfigOptionBeforePositionalArg){
    std::string configPath, jsonArg, error;

    const bool ok = callParseArgs({"--config", "custom.json", "{\"commands\":[]}"},
        configPath, jsonArg, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "custom.json");
    EXPECT_EQ(jsonArg, "{\"commands\":[]}");
}

TEST(ApplicationParseArgsTest, ConfigOptionAfterPositionalArg){
    std::string configPath, jsonArg, error;

    const bool ok = callParseArgs({"{\"commands\":[]}", "--config", "custom.json"},
        configPath, jsonArg, error);

    EXPECT_TRUE(ok);
    EXPECT_EQ(configPath, "custom.json");
    EXPECT_EQ(jsonArg, "{\"commands\":[]}");
}

TEST(ApplicationParseArgsTest, ConfigOptionWithoutValueFails){
    std::string configPath, jsonArg, error;

    const bool ok = callParseArgs({"--config"}, configPath, jsonArg, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

TEST(ApplicationParseArgsTest, UnknownOptionFails){
    std::string configPath, jsonArg, error;

    const bool ok = callParseArgs({"--replay", "foo.jsonl"}, configPath, jsonArg, error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "Unknown option: --replay");
}

TEST(ApplicationParseArgsTest, ExtraPositionalArgFails){
    std::string configPath, jsonArg, error;

    const bool ok = callParseArgs({"{\"commands\":[]}", "{\"commands\":[]}"},
        configPath, jsonArg, error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}
