#include <gtest/gtest.h>

#include "halcyon/detail/cli/driver.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Error;
using halcyon::ErrorCode;
using halcyon::detail::cli::ConnectionHandle;
using halcyon::detail::cli::ConnectionParams;
using halcyon::testing::MockCliDriver;

TEST(CliSeam, ConnectReturnsValidHandle) {
    MockCliDriver driver;
    auto r = driver.connect(ConnectionParams{"DATABASE=SAMPLE;"});
    ASSERT_TRUE(r.ok());
    EXPECT_NE(r.value(), ConnectionHandle::invalid);
    EXPECT_EQ(driver.connectCalls, 1);
    ASSERT_EQ(driver.connectParams.size(), 1u);
    EXPECT_EQ(driver.connectParams[0].connectionString, "DATABASE=SAMPLE;");
}

TEST(CliSeam, ConnectPropagatesScriptedError) {
    MockCliDriver driver;
    Error e;
    e.code = ErrorCode::Connection;
    e.sqlstate = "08001";
    e.retriable = true;
    driver.connectErrors.push_back(e);

    auto r = driver.connect(ConnectionParams{"bad"});
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Connection);
}

TEST(CliSeam, IsAliveHonorsScriptedResults) {
    MockCliDriver driver;
    driver.aliveResults = {false, true};
    auto h = driver.connect(ConnectionParams{"x"}).value();
    EXPECT_FALSE(driver.isAlive(h).value());
    EXPECT_TRUE(driver.isAlive(h).value());
    EXPECT_EQ(driver.aliveCalls, 2);
}

TEST(CliSeam, DisconnectCounts) {
    MockCliDriver driver;
    auto h = driver.connect(ConnectionParams{"x"}).value();
    ASSERT_TRUE(driver.disconnect(h).ok());
    EXPECT_EQ(driver.disconnectCalls, 1);
}
