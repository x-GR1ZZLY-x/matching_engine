#pragma once

// Общий помощник для интеграционных тестов, требующих реальной БД.
// Параметры подключения берутся из конфигурации приложения (см.
// config.hpp), а не из строковых литералов — иначе хост, имя базы
// и пользователь оседали бы в исходниках тестов.

#include<gtest/gtest.h>
#include<cstdlib>
#include<fstream>
#include<optional>
#include<string>
#include"config.hpp"
#include"exceptions.hpp"
#include"pg_connection.hpp"

namespace matching_engine{
namespace test{

// Причина последнего неудачного tryConnect() — выводится через GTEST_SKIP(),
// чтобы отличить "сервер не запущен" от опечатки в пароле или имени БД.
inline std::string g_lastConnectFailure = "БД ещё не проверялась";

// Путь к конфигурации приложения. ctest запускает тесты с рабочим каталогом
// build/, поэтому путь строится от MATCHING_ENGINE_SOURCE_DIR, а не
// относительно текущего каталога.
inline std::string configPath(){
    return std::string(MATCHING_ENGINE_SOURCE_DIR) + "/config/config.json";
}

// Пытается подключиться к тестовой базе, используя конфигурацию приложения
// и пароль из переменной окружения MATCHING_ENGINE_DB_PASSWORD. Возвращает
// nullopt, если БД недоступна, конфигурация не читается или пароль не
// задан — тогда интеграционные тесты пропускаются.
inline std::optional<PgConnection> tryConnect(){
    // Два законных повода пропустить тест — нет файла конфигурации и не задан
    // пароль, то есть машина без локальной настройки БД. Оба проверяются
    // здесь напрямую, до разбора конфигурации. Всё остальное, чем может
    // ответить loadConfig (невалидный JSON, отсутствующая секция или поле
    // в существующем файле), — ошибка конфигурации, а не недоступность
    // базы: такую поломку GTEST_SKIP() маскировать не должен, иначе
    // опечатка в одном поле молча уводит в пропуск все тесты, требующие БД,
    // и ctest возвращает успех, ничего не проверив.
    // Проверки именно прямые, а не по тексту сообщения ConfigError:
    // формулировки принадлежат config.cpp и вольны меняться, а
    // ценой такой связи был бы молчаливый пропуск всего слоя персистентности.
    {
        std::ifstream configFile(configPath());
        if(!configFile.is_open()){
            g_lastConnectFailure = "не открывается файл конфигурации: " + configPath();
            return std::nullopt;
        }
    }

    if(const char* password = std::getenv("MATCHING_ENGINE_DB_PASSWORD");
        !password || password[0] == '\0'){
        g_lastConnectFailure = "переменная MATCHING_ENGINE_DB_PASSWORD не задана";
        return std::nullopt;
    }

    const DatabaseConfig config = loadConfig(configPath()).database;

    try{
        return PgConnection(config.host, config.port, config.dbname,
            config.user, config.password);
    } catch(const std::exception& ex){
        g_lastConnectFailure = ex.what();
        return std::nullopt;
    }
}

}
}
