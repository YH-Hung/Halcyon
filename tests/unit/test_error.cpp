#include <gtest/gtest.h>

#include "halcyon/error.hpp"

using halcyon::Error;
using halcyon::ErrorCode;

TEST(ErrorModel, ToStringCoversAllCodes) {
    EXPECT_STREQ(halcyon::to_string(ErrorCode::Connection), "Connection");
    EXPECT_STREQ(halcyon::to_string(ErrorCode::Deadlock), "Deadlock");
    EXPECT_STREQ(halcyon::to_string(ErrorCode::Unknown), "Unknown");
}

TEST(ErrorModel, ThrowErrorMapsCodeToSubtype) {
    Error e;
    e.code = ErrorCode::Constraint;
    e.sqlstate = "23505";
    e.message = "duplicate key";
    EXPECT_THROW(halcyon::throw_error(e), halcyon::ConstraintException);
}

TEST(ErrorModel, ExceptionCarriesErrorAndMessage) {
    Error e;
    e.code = ErrorCode::Connection;
    e.sqlstate = "08001";
    e.message = "cannot connect";
    e.retriable = true;
    try {
        halcyon::throw_error(e);
        FAIL() << "expected throw";
    } catch (const halcyon::Exception& ex) {
        EXPECT_EQ(ex.error().code, ErrorCode::Connection);
        EXPECT_EQ(ex.error().sqlstate, "08001");
        EXPECT_TRUE(ex.error().retriable);
        EXPECT_STREQ(ex.what(), "cannot connect");
    }
}
