#include "sequence_generator.hpp"

namespace matching_engine{

SequenceGenerator::SequenceGenerator() : next_(1){}

SequenceGenerator& SequenceGenerator::instance(){
    static SequenceGenerator generator;
    return generator;
}

long long SequenceGenerator::next(){
    return next_++;
}

void SequenceGenerator::reset(long long value){
    next_ = value;
}

}
