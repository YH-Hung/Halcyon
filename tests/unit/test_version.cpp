#include <gtest/gtest.h>

#include "halcyon/version.hpp"

TEST(Version, ReturnsSemanticVersionString) {
    EXPECT_EQ(halcyon::version(), "1.0.0");
}

TEST(Version, ConstantsMatchString) {
    EXPECT_EQ(halcyon::version_major, 1);
    EXPECT_EQ(halcyon::version_minor, 0);
    EXPECT_EQ(halcyon::version_patch, 0);
}

TEST(Version, StringConstantMatchesFunction) {
    EXPECT_EQ(halcyon::version(), halcyon::version_string);
}
