#include<gtest/gtest.h>
#include<cstdint>
#include<cstdlib>
#include<filesystem>
#include<fstream>
#include<limits>
#include<string>
#include"config.hpp"
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

constexpr const char* kValidConfig = R"({
    "server": {
        "address": "0.0.0.0",
        "port": 9000,
        "max_message_size": 1048576
    },
    "database": {
        "host": "db.example.test",
        "port": 6543,
        "name": "some_db",
        "user": "some_user",
        "schema_dir": "database"
    }
})";

}

TEST(ConfigTest, LoadsValidConfigWithPasswordFromEnv){
    const ScopedTempFile tempFile("valid", kValidConfig);
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    const AppConfig config = loadConfig(tempFile.path());

    EXPECT_EQ(config.server.address, "0.0.0.0");
    EXPECT_EQ(config.server.port, 9000);
    EXPECT_EQ(config.server.maxMessageSize, static_cast<std::size_t>(1048576));

    EXPECT_EQ(config.database.host, "db.example.test");
    EXPECT_EQ(config.database.port, "6543");
    EXPECT_EQ(config.database.dbname, "some_db");
    EXPECT_EQ(config.database.user, "some_user");
    EXPECT_EQ(config.database.password, "s3cret");
    EXPECT_EQ(config.database.schemaDir, "database");
}

TEST(ConfigTest, MissingFileThrowsConfigError){
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    EXPECT_THROW(loadConfig("/nonexistent/path/matching_engine_no_such_file.json"),
        ConfigError);
}

TEST(ConfigTest, InvalidJsonThrowsConfigError){
    const ScopedTempFile tempFile("invalid_json", "{ this is not json");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    EXPECT_THROW(loadConfig(tempFile.path()), ConfigError);
}

TEST(ConfigTest, MissingServerSectionThrowsConfigError){
    const ScopedTempFile tempFile("missing_server_section", R"({
        "database": {
            "host": "db.example.test",
            "port": 6543,
            "name": "some_db",
            "user": "some_user"
        }
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    EXPECT_THROW(loadConfig(tempFile.path()), ConfigError);
}

TEST(ConfigTest, MissingDatabaseSectionThrowsConfigError){
    const ScopedTempFile tempFile("missing_database_section", R"({
        "server": {
            "address": "0.0.0.0",
            "port": 9000,
            "max_message_size": 1048576
        }
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    EXPECT_THROW(loadConfig(tempFile.path()), ConfigError);
}

TEST(ConfigTest, MissingRequiredFieldThrowsConfigError){
    const ScopedTempFile tempFile("missing_field", R"({
        "server": {
            "address": "0.0.0.0",
            "port": 9000,
            "max_message_size": 1048576
        },
        "database": {
            "host": "db.example.test",
            "port": 6543,
            "name": "some_db"
        }
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    try{
        loadConfig(tempFile.path());
        FAIL() << "loadConfig must throw when a required field is missing";
    } catch(const ConfigError& e){
        EXPECT_NE(std::string(e.what()).find("database.user"), std::string::npos);
    }
}

// Путь к каталогу схемы обязателен в секции "database": под управлением
// службы рабочий каталог не совпадает с каталогом сборки, поэтому
// относительный путь в коде не годится. Отсутствие поля даёт внятную ошибку
// из иерархии MatchingEngineError (ConfigError), а не падение или
// неопределённое поведение при последующей попытке применить схему по
// пустому пути.
TEST(ConfigTest, MissingSchemaDirThrowsConfigError){
    const ScopedTempFile tempFile("missing_schema_dir", R"({
        "server": {
            "address": "0.0.0.0",
            "port": 9000,
            "max_message_size": 1048576
        },
        "database": {
            "host": "db.example.test",
            "port": 6543,
            "name": "some_db",
            "user": "some_user"
        }
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    try{
        loadConfig(tempFile.path());
        FAIL() << "loadConfig must throw when database.schema_dir is missing";
    } catch(const ConfigError& e){
        EXPECT_NE(std::string(e.what()).find("database.schema_dir"), std::string::npos);
    } catch(const MatchingEngineError&){
        FAIL() << "loadConfig must throw ConfigError specifically, not another "
            "MatchingEngineError subtype";
    }
}

TEST(ConfigTest, MissingPasswordEnvVarThrowsConfigError){
    const ScopedTempFile tempFile("missing_password", kValidConfig);
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", nullptr);

    EXPECT_THROW(loadConfig(tempFile.path()), ConfigError);
}

TEST(ConfigTest, EmptyPasswordEnvVarThrowsConfigError){
    const ScopedTempFile tempFile("empty_password", kValidConfig);
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "");

    EXPECT_THROW(loadConfig(tempFile.path()), ConfigError);
}

TEST(ConfigTest, PortAsStringThrowsConfigError){
    const ScopedTempFile tempFile("port_as_string", R"({
        "server": {
            "address": "0.0.0.0",
            "port": "9000",
            "max_message_size": 1048576
        },
        "database": {
            "host": "db.example.test",
            "port": 6543,
            "name": "some_db",
            "user": "some_user"
        }
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    EXPECT_THROW(loadConfig(tempFile.path()), ConfigError);
}

// Значение вне диапазона TCP-портов обязано быть отвергнуто, а не усечено при
// сужении типа: 4294967296 отбрасыванием старших разрядов превращается в 0,
// и вместо ошибки конфигурации получился бы сервер на случайном порту.
TEST(ConfigTest, PortAboveMaximumThrowsConfigError){
    const ScopedTempFile tempFile("port_above_maximum", R"({
        "server": {
            "address": "0.0.0.0",
            "port": 4294967296,
            "max_message_size": 1048576
        },
        "database": {
            "host": "db.example.test",
            "port": 6543,
            "name": "some_db",
            "user": "some_user"
        }
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    EXPECT_THROW(loadConfig(tempFile.path()), ConfigError);
}

// Нулевой порт — законное значение, а не ошибка: по нему порт выбирает
// операционная система, и на этом держатся сетевые тесты.
TEST(ConfigTest, ZeroPortIsAccepted){
    const ScopedTempFile tempFile("zero_port", R"({
        "server": {
            "address": "127.0.0.1",
            "port": 0,
            "max_message_size": 1048576
        },
        "database": {
            "host": "db.example.test",
            "port": 6543,
            "name": "some_db",
            "user": "some_user",
            "schema_dir": "database"
        }
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    const AppConfig config = loadConfig(tempFile.path());

    EXPECT_EQ(config.server.port, 0);
}

// Ноль запретил бы любое непустое сообщение — max_message_size обязан быть
// не меньше единицы.
TEST(ConfigTest, ZeroMaxMessageSizeThrowsConfigError){
    const ScopedTempFile tempFile("zero_max_message_size", R"({
        "server": {
            "address": "0.0.0.0",
            "port": 9000,
            "max_message_size": 0
        },
        "database": {
            "host": "db.example.test",
            "port": 6543,
            "name": "some_db",
            "user": "some_user"
        }
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    EXPECT_THROW(loadConfig(tempFile.path()), ConfigError);
}

// Значение больше UINT32_MAX не отловится decodeHeader ни при каком
// заголовке (заголовок сам не может объявить больше UINT32_MAX) и тихо
// снимет защиту от заявленного гиганта — конфигурация обязана отвергнуть
// такое значение сама.
TEST(ConfigTest, MaxMessageSizeAboveUint32MaxThrowsConfigError){
    const ScopedTempFile tempFile("max_message_size_above_uint32_max", R"({
        "server": {
            "address": "0.0.0.0",
            "port": 9000,
            "max_message_size": 4294967296
        },
        "database": {
            "host": "db.example.test",
            "port": 6543,
            "name": "some_db",
            "user": "some_user"
        }
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    EXPECT_THROW(loadConfig(tempFile.path()), ConfigError);
}

// Граничное значение, равное UINT32_MAX, — законный максимум и обязано
// приниматься, а не отвергаться вместе со значениями выше предела.
TEST(ConfigTest, MaxMessageSizeAtUint32MaxIsAccepted){
    const ScopedTempFile tempFile("max_message_size_at_uint32_max", R"({
        "server": {
            "address": "0.0.0.0",
            "port": 9000,
            "max_message_size": 4294967295
        },
        "database": {
            "host": "db.example.test",
            "port": 6543,
            "name": "some_db",
            "user": "some_user",
            "schema_dir": "database"
        }
    })");
    ScopedEnv env("MATCHING_ENGINE_DB_PASSWORD", "s3cret");

    const AppConfig config = loadConfig(tempFile.path());

    EXPECT_EQ(config.server.maxMessageSize,
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
}
