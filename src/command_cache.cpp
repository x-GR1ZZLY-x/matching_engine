#include"command_cache.hpp"

namespace matching_engine{

const ExecutionResult* CommandCache::find(const std::string& commandId) const{
    auto it = results_.find(commandId);
    return it == results_.end() ? nullptr : &it->second;
}

void CommandCache::put(const std::string& commandId, ExecutionResult result){
    results_[commandId] = std::move(result);
}

}
