#pragma once

#include "order.hpp"

namespace matching_engine{

enum class CommandType{
    Add,
    Cancel,
    Print
};

struct Command{
    explicit Command(CommandType type) : type_(type) {}

    virtual ~Command() = default;

    Command(const Command&) = delete;

    Command& operator=(const Command&) = delete;

    const CommandType type_;
};

struct AddCommand : Command {
    AddCommand(int id, Side side, int price, int quantity)
        : Command(CommandType::Add),
        id_(id),
        side_(side),
        price_(price),
        quantity_(quantity) {}

    int id_;
    Side side_;
    int price_;
    int quantity_;
};

struct CancelCommand : Command {
    explicit CancelCommand(int id)
        : Command(CommandType::Cancel),
        id_(id) {}

    int id_;
};

struct PrintCommand : Command {
    PrintCommand() : Command(CommandType::Print) {}
};

}