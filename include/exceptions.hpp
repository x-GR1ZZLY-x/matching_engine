#pragma once

#include <stdexcept>
#include <string>

namespace matching_engine {

class MatchingEngineError : public std::runtime_error {
public:
    explicit MatchingEngineError(const std::string& message)
        : std::runtime_error(message) {}
};

class OrderError : public MatchingEngineError{
public:
    explicit OrderError(const std::string& message)
        : MatchingEngineError(message) {}
};

class ParseError : public MatchingEngineError{
public:
    explicit ParseError(const std::string& message)
        : MatchingEngineError(message) {}
};

class DuplicateOrderError : public MatchingEngineError{
public:
    explicit DuplicateOrderError(const std::string& message)
        : MatchingEngineError(message) {}
};

class OrderBookError : public MatchingEngineError{
public:
    explicit OrderBookError(const std::string& message)
        : MatchingEngineError(message) {}
};

class DatabaseError : public MatchingEngineError{
public:
    explicit DatabaseError(const std::string& message)
        : MatchingEngineError(message) {}
};

}