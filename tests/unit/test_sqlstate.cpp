#include <gtest/gtest.h>

#include "halcyon/detail/cli/sqlstate.hpp"

using halcyon::ErrorCode;
using halcyon::detail::cli::classify_sqlstate;

TEST(SqlState, ConnectionClassIsRetriable) {
    auto c = classify_sqlstate("08001", 0);
    EXPECT_EQ(c.code, ErrorCode::Connection);
    EXPECT_TRUE(c.retriable);
}

TEST(SqlState, CommLostNativeCodeIsTransient) {
    auto c = classify_sqlstate("", -30081);
    EXPECT_EQ(c.code, ErrorCode::Transient);
    EXPECT_TRUE(c.retriable);
}

TEST(SqlState, DeadlockAndLockTimeout) {
    EXPECT_EQ(classify_sqlstate("40001", 0).code, ErrorCode::Deadlock);
    EXPECT_TRUE(classify_sqlstate("40001", 0).retriable);
    EXPECT_EQ(classify_sqlstate("57033", 0).code, ErrorCode::Timeout);
    EXPECT_TRUE(classify_sqlstate("57033", 0).retriable);
}

TEST(SqlState, ConstraintAndSyntaxAreNotRetriable) {
    EXPECT_EQ(classify_sqlstate("23505", 0).code, ErrorCode::Constraint);
    EXPECT_FALSE(classify_sqlstate("23505", 0).retriable);
    EXPECT_EQ(classify_sqlstate("42601", 0).code, ErrorCode::Syntax);
    EXPECT_FALSE(classify_sqlstate("42601", 0).retriable);
}

TEST(SqlState, UnknownDefault) {
    auto c = classify_sqlstate("99999", 0);
    EXPECT_EQ(c.code, ErrorCode::Unknown);
    EXPECT_FALSE(c.retriable);
}
