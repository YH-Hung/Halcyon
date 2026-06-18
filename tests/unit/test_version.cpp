#include <gtest/gtest.h>

#include "halcyon/version.hpp"

TEST(Version, ReturnsSemanticVersionString) {
    EXPECT_EQ(halcyon::version(), "0.1.0");
}

TEST(Version, ConstantsMatchString) {
    EXPECT_EQ(halcyon::version_major, 0);
    EXPECT_EQ(halcyon::version_minor, 1);
    EXPECT_EQ(halcyon::version_patch, 0);
}
