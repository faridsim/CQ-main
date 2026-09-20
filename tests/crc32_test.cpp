#include "cq/crc32.hpp"

#include <gtest/gtest.h>

TEST(Crc32, SameInputSameDigest)
{
    EXPECT_EQ(cq::computeCrc(42), cq::computeCrc(42));
}

TEST(Crc32, DifferentInputDifferentDigest)
{
    EXPECT_NE(cq::computeCrc(42), cq::computeCrc(43));
}