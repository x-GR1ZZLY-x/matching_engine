#pragma once

#include<string>
#include"pg_connection.hpp"

namespace matching_engine{

// Читает файлы *.sql из schemaDir в лексическом порядке имён (001_init.sql
// до 002_indexes.sql и так далее) и выполняет их содержимое через
// соединение. Каждый файл может содержать несколько ';'-разделённых
// операторов. Все операторы схемы идемпотентны (CREATE ... IF NOT EXISTS),
// поэтому повторный вызов на уже применённой схеме безопасен.
// Бросает DatabaseError, если каталог не найден, недоступен, не содержит
// *.sql файлов или один из операторов завершился ошибкой.
void applySchema(PgConnection& connection, const std::string& schemaDir);

}
