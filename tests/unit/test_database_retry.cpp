#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>

#include "capturing_logger.hpp"
#include "halcyon/database.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::ErrorCode;
using halcyon::ExecPolicy;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;

namespace {
halcyon::Error transientExec() {
    halcyon::Error e;
    e.code = ErrorCode::Transient;
    e.retriable = true;
    e.message = "comm lost";
    return e;
}
PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    c.backoff.sleep = [](std::chrono::milliseconds) {};
    return c;
}
MockCliDriver::ScriptedRows oneInt(std::int64_t v) {
    return MockCliDriver::ScriptedRows{{"c"}, {{halcyon::detail::cli::Value{v}}}};
}
}  // namespace

TEST(DatabaseRetry, ReadOnlyQueryRetriedOnTransientError) {
    MockCliDriver driver;
    driver.executeErrors.push_back(transientExec());  // first execute fails
    driver.resultSets.push_back(oneInt(5));           // retry succeeds
    auto db = Database::open(driver, "X", noThread()).value();

    auto qr = db.query("SELECT c FROM t");
    ASSERT_TRUE(qr.ok()) << qr.error().message;
    std::int64_t sum = 0;
    for (auto& row : qr.value()) sum += std::get<0>(row.as<std::int64_t>());
    EXPECT_EQ(sum, 5);
}

TEST(DatabaseRetry, WriteNotRetriedByDefault) {
    MockCliDriver driver;
    driver.executeErrors.push_back(transientExec());
    driver.execRowCounts.push_back(1);  // would succeed if retried
    auto db = Database::open(driver, "X", noThread()).value();

    auto n = db.execute("INSERT INTO t VALUES (?)", 1);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, ErrorCode::Transient);
}

TEST(DatabaseRetry, WriteRetriedWhenIdempotentPolicyGiven) {
    MockCliDriver driver;
    driver.executeErrors.push_back(transientExec());
    driver.execRowCounts.push_back(1);
    auto db = Database::open(driver, "X", noThread()).value();

    auto n = db.execute("INSERT INTO t VALUES (?)", ExecPolicy::idempotent(3), 1);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 1);
}

TEST(DatabaseRetry, NonRetriableErrorNotRetried) {
    MockCliDriver driver;
    halcyon::Error syntax;
    syntax.code = ErrorCode::Syntax;
    syntax.retriable = false;
    driver.executeErrors.push_back(syntax);
    driver.resultSets.push_back(oneInt(5));
    auto db = Database::open(driver, "X", noThread()).value();

    auto qr = db.query("SELECT c FROM t");
    ASSERT_FALSE(qr.ok());
    EXPECT_EQ(qr.error().code, ErrorCode::Syntax);
}

TEST(DatabaseRetry, ConnectionErrorMarksLeaseBroken) {
    MockCliDriver driver;
    halcyon::Error connErr;
    connErr.code = ErrorCode::Connection;
    connErr.retriable = true;
    driver.executeErrors.push_back(connErr);
    driver.resultSets.push_back(oneInt(9));
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    auto db = Database::open(driver, "X", cfg).value();

    auto qr = db.query("SELECT c FROM t");
    ASSERT_TRUE(qr.ok()) << qr.error().message;
    // broken lease was discarded and a replacement connected
    EXPECT_GE(driver.connectCalls, 2);
}

struct RetryCountRow {
    std::int64_t c;
};
HALCYON_REFLECT(RetryCountRow, c);

TEST(DatabaseRetryLogging, RetriedAttemptIsLogged) {
    halcyon::testing::MockCliDriver drv;
    auto logger = std::make_shared<halcyon::testing::CapturingLogger>();
    halcyon::PoolConfig cfg;
    cfg.min = 1;
    cfg.max = 2;
    cfg.startMaintenanceThread = false;
    cfg.backoff.maxAttempts = 2;
    cfg.backoff.sleep = [](std::chrono::milliseconds) {};  // no real waiting
    cfg.observability.logger = logger;
    auto db = halcyon::Database::open(drv, "dsn", cfg);
    ASSERT_TRUE(db.ok());
    halcyon::Error transient;
    transient.code = halcyon::ErrorCode::Transient;
    transient.retriable = true;
    drv.executeErrors.push_back(transient);  // first SELECT attempt fails
    auto r = db.value().queryAs<RetryCountRow>("SELECT 1 FROM t");
    // Second attempt succeeds (no more scripted errors).
    EXPECT_EQ(logger->count("retry.attempt"), 1u);
}
