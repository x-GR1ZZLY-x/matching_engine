#include<gtest/gtest.h>
#include<cstdlib>
#include<filesystem>
#include<fstream>
#include<string>
#include"database_config.hpp"
#include"exceptions.hpp"

using namespace matching_engine;

namespace {

// RAII-хелпер: пишет содержимое во временный файл с уникальным именем и
// удаляет файл при разрушении объекта.
class ScopedTempFile {
public:
    ScopedTempFile(const std::string& name, const std::string& content)
        : path_(std::filesystem::temp_directory_path() /
              ("matching_engine_test_" + name + "_" + std::to_string(nextId()) + ".json")){
        std::ofstream file(path_);
        file << content;
        if(!file){
            throw std::runtime_error("Failed to write temp file: " + path_.string());
        }
    }

    ~ScopedTempFile(){
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;

    std::string path() const { return path_.string(); }

private:
    static int nextId(){
        static int counter = 0;
        return counter++;
    }

    std::filesystem::path path_;
};

// RAII-помощник: устанавливает переменную окружения на время теста и
// восстанавливает предыдущее значение (или отсутствие) после.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name){
        const char* previous = std::getenv(name);
        hadPrevious_ = previous != nullptr;
        if(hadPrevious_) previousValue_ = previous;

        if(value){
            setenv(name_.c_str(), value, 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    ~ScopedEnv(){
        if(hadPrevious_){
            setenv(name_.c_str(), previousValue_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
    bool hadPrevious_;
    std::string previousValue_;
};

}

TEST(DatabaseConfigTest, LoadsValidConfigWithPasswordFromEnv){
    const ScopedTempFile tempFile("valid", R"({
        "host": "db.example.test",
        "port": "6543",
        "dbname": "some_db",
        "user": "some_user"
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    const DatabaseConfig config = loadDatabaseConfig(tempFile.path());

    EXPECT_EQ(config.host, "db.example.test");
    EXPECT_EQ(config.port, "6543");
    EXPECT_EQ(config.dbname, "some_db");
    EXPECT_EQ(config.user, "some_user");
    EXPECT_EQ(config.password, "s3cret");
}

TEST(DatabaseConfigTest, MissingFileThrowsConfigError){
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    EXPECT_THROW(loadDatabaseConfig("/nonexistent/path/matching_engine_no_such_file.json"),
        ConfigError);
}

TEST(DatabaseConfigTest, InvalidJsonThrowsConfigError){
    const ScopedTempFile tempFile("invalid_json", "{ this is not json");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    EXPECT_THROW(loadDatabaseConfig(tempFile.path()), ConfigError);
}

TEST(DatabaseConfigTest, MissingRequiredFieldThrowsConfigError){
    const ScopedTempFile tempFile("missing_field", R"({
        "host": "db.example.test",
        "port": "6543",
        "dbname": "some_db"
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    EXPECT_THROW(loadDatabaseConfig(tempFile.path()), ConfigError);
}

TEST(DatabaseConfigTest, MissingPasswordEnvVarThrowsConfigError){
    const ScopedTempFile tempFile("missing_password", R"({
        "host": "db.example.test",
        "port": "6543",
        "dbname": "some_db",
        "user": "some_user"
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", nullptr);

    EXPECT_THROW(loadDatabaseConfig(tempFile.path()), ConfigError);
}

TEST(DatabaseConfigTest, EmptyPasswordEnvVarThrowsConfigError){
    const ScopedTempFile tempFile("empty_password", R"({
        "host": "db.example.test",
        "port": "6543",
        "dbname": "some_db",
        "user": "some_user"
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "");

    EXPECT_THROW(loadDatabaseConfig(tempFile.path()), ConfigError);
}
