#include "cq/crc32.hpp"
#include "test_types.hpp"

#include <gtest/gtest.h>

TEST(Crc32, SameInputSameDigest)
{
    EXPECT_EQ(cq::computeCrc(42), cq::computeCrc(42));
    EXPECT_NE(cq::computeCrc(42), cq::computeCrc(43));
}

TEST(Crc32, StructCoversAllFields)
{
    Data a{1U, 2U, 3U};
    Data b{1U, 2U, 4U};
    EXPECT_NE(cq::computeCrc(a), cq::computeCrc(b));
}
