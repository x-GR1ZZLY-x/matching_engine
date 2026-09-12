#pragma once

#include<string>
#include<unordered_map>
#include"execution_result.hpp"

namespace matching_engine{

// In-memory отображение command_id -> результат выполнения команды
// (REQ-IDEM-04). Размер не ограничен: каждая обработанная изменяющая
// команда остаётся в кеше до конца процесса. О БД, JSON и остальных слоях
// приложения не знает — простая карта поверх ExecutionResult.
//
// Прогрев кеша на старте — обычные вызовы put() с записями, прочитанными из
// processed_commands при старте; отдельного метода массовой загрузки не
// требуется, put() уже умеет и это, и обычную запись при первой обработке
// команды.
class CommandCache{
public:
    // Указатель на закешированный результат либо nullptr, если command_id
    // ещё не встречался. std::unordered_map не перемещает уже вставленные
    // элементы при rehash (инвалидируются только итераторы), поэтому
    // указатель остаётся годным и после последующих put() с другими ключами;
    // повторный put() с тем же ключом, разумеется, заменяет значение под
    // ранее выданным указателем.
    const ExecutionResult* find(const std::string& commandId) const;

    void put(const std::string& commandId, ExecutionResult result);

private:
    std::unordered_map<std::string, ExecutionResult> results_;
};

}
