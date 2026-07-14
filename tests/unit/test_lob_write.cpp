#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "halcyon/halcyon.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Connection;
using halcyon::ErrorCode;
using halcyon::testing::MockCliDriver;

namespace {

std::vector<std::byte> bytes_of(const std::string& s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

}  // namespace

TEST(LobWrite, ConnectionExecuteRoutesThroughExecuteStreaming) {
    MockCliDriver drv;
    auto cr = Connection::open(drv, {"dsn"});
    Connection conn = std::move(cr.value());
    std::istringstream doc("document bytes");
    auto r = conn.execute("INSERT INTO docs(id, content) VALUES (?, ?)",
                          std::int64_t{7}, halcyon::lobStream(doc));
    ASSERT_TRUE(r.ok()) << r.error().message;
    EXPECT_EQ(drv.executeStreamingCalls, 1);
    ASSERT_EQ(drv.lastStreamValues.size(), 2u);
    EXPECT_EQ(std::get<std::int64_t>(drv.lastStreamValues[0]), 7);
    EXPECT_TRUE(std::holds_alternative<halcyon::detail::cli::Null>(
        drv.lastStreamValues[1]));
    EXPECT_EQ(drv.lastStreamedLobs[1], bytes_of("document bytes"));
}

TEST(LobWrite, StreamingFailurePoisonsLease) {
    MockCliDriver drv;
    auto cr = Connection::open(drv, {"dsn"}, /*statementCacheSize=*/8);
    Connection conn = std::move(cr.value());
    halcyon::Error e;
    e.code = ErrorCode::Connection;
    drv.executeStreamingErrors.push_back(e);
    std::istringstream doc("x");
    auto r = conn.execute("INSERT INTO docs(content) VALUES (?)",
                          halcyon::lobStream(doc));
    ASSERT_FALSE(r.ok());
    EXPECT_GE(drv.finalizeCalls, 1);  // poisoned lease was dropped, not cached
}

TEST(LobWrite, TransactionForwards) {
    MockCliDriver drv;
    auto cr = Connection::open(drv, {"dsn"});
    Connection conn = std::move(cr.value());
    auto tx = conn.begin();
    ASSERT_TRUE(tx.ok());
    std::istringstream doc("in txn");
    auto r = tx.value().execute("INSERT INTO docs(content) VALUES (?)",
                                halcyon::lobStream(doc));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(drv.executeStreamingCalls, 1);
    ASSERT_TRUE(tx.value().commit().ok());
}

TEST(LobWrite, DatabaseExecuteNeverRetriesStreams) {
    MockCliDriver drv;
    halcyon::PoolConfig cfg;
    cfg.min = 1;
    cfg.max = 2;
    cfg.startMaintenanceThread = false;
    cfg.backoff.maxAttempts = 3;
    auto db = halcyon::Database::open(drv, "dsn", cfg);
    ASSERT_TRUE(db.ok());
    halcyon::Error transient;
    transient.code = ErrorCode::Transient;
    transient.retriable = true;  // would be retried if this were a plain read
    drv.executeStreamingErrors.push_back(transient);
    std::istringstream doc("payload");
    auto r = db.value().execute("INSERT INTO docs(content) VALUES (?)",
                                halcyon::lobStream(doc));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(drv.executeStreamingCalls, 1);  // exactly one attempt
}

TEST(LobWrite, ThrowingCallbackIsMappingError) {
    MockCliDriver drv;
    auto cr = Connection::open(drv, {"dsn"}, /*statementCacheSize=*/8);
    Connection conn = std::move(cr.value());
    auto r = conn.execute(
        "INSERT INTO docs(content) VALUES (?)",
        halcyon::lobCallback([](std::byte*, std::size_t) -> std::size_t {
            throw std::runtime_error("boom");
        }));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Mapping);
}

TEST(LobWrite, OversizedChunkIsMappingError) {
    MockCliDriver drv;
    auto cr = Connection::open(drv, {"dsn"}, /*statementCacheSize=*/8);
    Connection conn = std::move(cr.value());
    auto r = conn.execute(
        "INSERT INTO docs(content) VALUES (?)",
        halcyon::lobCallback([](std::byte*, std::size_t cap) -> std::size_t {
            return cap + 1;  // claims more than the buffer holds
        }));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, ErrorCode::Mapping);
}
