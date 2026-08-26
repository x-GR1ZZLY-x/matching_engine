#include <gtest/gtest.h>
#include "sequence_generator.hpp"

using namespace matching_engine;

// Счётчик — общее состояние на процесс, поэтому каждый тест выставляет его
// в известное значение в начале, а не полагается на порядок запуска.

TEST(SequenceGeneratorTest, ConsecutiveCallsAreStrictlyIncreasing)
{
    SequenceGenerator::instance().reset(1);

    long long first = SequenceGenerator::instance().next();
    long long second = SequenceGenerator::instance().next();

    EXPECT_LT(first, second);
}

TEST(SequenceGeneratorTest, ResetMakesNextCallReturnGivenValue)
{
    SequenceGenerator::instance().reset(500);

    EXPECT_EQ(SequenceGenerator::instance().next(), 500);
}

TEST(SequenceGeneratorTest, ResetAffectsOnlyFollowingCalls)
{
    SequenceGenerator::instance().reset(10);

    EXPECT_EQ(SequenceGenerator::instance().next(), 10);
    EXPECT_EQ(SequenceGenerator::instance().next(), 11);
    EXPECT_EQ(SequenceGenerator::instance().next(), 12);
}
