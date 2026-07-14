#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "halcyon/halcyon.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Connection;
using halcyon::ErrorCode;
using halcyon::testing::MockCliDriver;
using Value = halcyon::detail::cli::Value;
using Null = halcyon::detail::cli::Null;

namespace {

struct Fixture {
    MockCliDriver drv;
    std::unique_ptr<Connection> conn;
    explicit Fixture(std::size_t cacheSize = 0) {
        auto c = Connection::open(drv, {"dsn"}, cacheSize);
        conn = std::make_unique<Connection>(std::move(c.value()));
    }
};

// Records "release" whenever the pool emits its connections gauge — i.e. when a
// lease returns to the pool. Combined with the mock's onCloseCursor/onRollback
// probes, a test can assert the cursor/transaction is torn down BEFORE the
// connection returns (the ordering the custom move-assignments guarantee).
struct OrderSink final : halcyon::obs::MetricsSink {
    std::vector<std::string>* log;
    explicit OrderSink(std::vector<std::string>* l) : log(l) {}
    void counter(std::string_view, double, const halcyon::obs::Labels&) override {}
    void histogram(std::string_view, double,
                   const halcyon::obs::Labels&) override {}
    void gauge(std::string_view name, double,
               const halcyon::obs::Labels&) override {
        if (name == "halcyon_pool_connections") log->push_back("release");
    }
};

std::string read_all(halcyon::LobReader& lob, std::size_t bufSize) {
    std::string out;
    std::vector<std::byte> buf(bufSize);
    for (;;) {
        auto n = lob.read(buf.data(), buf.size());
        EXPECT_TRUE(n.ok());
        if (!n.ok() || n.value() == 0) break;
        out.append(reinterpret_cast<const char*>(buf.data()), n.value());
    }
    return out;
}

}  // namespace

TEST(Streaming, IteratesRowsWithScalarsAndLobs) {
    Fixture f;
    f.drv.resultSets.push_back(
        {{"id", "doc"},
         {{Value{std::int64_t{1}}, Value{std::string{"first blob"}}},
          {Value{std::int64_t{2}}, Value{std::string{"second blob"}}}}});
    f.drv.lobChunkCap = 4;  // force multiple chunks per cell
    auto rs = f.conn->queryStreaming("SELECT id, doc FROM docs");
    ASSERT_TRUE(rs.ok()) << rs.error().message;
    std::vector<std::pair<std::int64_t, std::string>> got;
    while (auto row = rs.value().next()) {
        auto id = row->get<std::int64_t>(0);
        ASSERT_TRUE(id.ok());
        auto lob = row->lob(1);
        ASSERT_TRUE(lob.ok());
        got.emplace_back(id.value(), read_all(lob.value(), 4));
    }
    EXPECT_TRUE(rs.value().ok());
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], (std::pair<std::int64_t, std::string>{1, "first blob"}));
    EXPECT_EQ(got[1], (std::pair<std::int64_t, std::string>{2, "second blob"}));
}

TEST(Streaming, OutOfOrderColumnAccessIsInvalidState) {
    Fixture f;
    f.drv.resultSets.push_back(
        {{"a", "b"}, {{Value{std::int64_t{1}}, Value{std::int64_t{2}}}}});
    auto rs = f.conn->queryStreaming("SELECT a, b FROM t");
    ASSERT_TRUE(rs.ok());
    auto row = rs.value().next();
    ASSERT_TRUE(row.has_value());
    ASSERT_TRUE(row->get<std::int64_t>(1).ok());
    auto again = row->get<std::int64_t>(1);  // same column: not ascending
    ASSERT_FALSE(again.ok());
    EXPECT_EQ(again.error().code, ErrorCode::InvalidState);
    auto earlier = row->lob(0);
    ASSERT_FALSE(earlier.ok());
    EXPECT_EQ(earlier.error().code, ErrorCode::InvalidState);
}

TEST(Streaming, NullLobReadsAsEofWithIsNull) {
    Fixture f;
    f.drv.resultSets.push_back({{"doc"}, {{Value{Null{}}}}});
    auto rs = f.conn->queryStreaming("SELECT doc FROM t");
    ASSERT_TRUE(rs.ok());
    auto row = rs.value().next();
    ASSERT_TRUE(row.has_value());
    auto lob = row->lob(0);
    ASSERT_TRUE(lob.ok());
    std::byte buf[8];
    auto n = lob.value().read(buf, sizeof(buf));
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 0u);
    EXPECT_TRUE(lob.value().isNull());
}

TEST(Streaming, EmptyLobIsEofWithoutNull) {
    Fixture f;
    f.drv.resultSets.push_back({{"doc"}, {{Value{std::string{}}}}});
    auto rs = f.conn->queryStreaming("SELECT doc FROM t");
    ASSERT_TRUE(rs.ok());
    auto row = rs.value().next();
    auto lob = row->lob(0);
    ASSERT_TRUE(lob.ok());
    std::byte buf[8];
    auto n = lob.value().read(buf, sizeof(buf));
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 0u);
    EXPECT_FALSE(lob.value().isNull());
}

TEST(Streaming, ExactBufferMultipleTerminates) {
    Fixture f;
    f.drv.resultSets.push_back({{"doc"}, {{Value{std::string{"12345678"}}}}});
    auto rs = f.conn->queryStreaming("SELECT doc FROM t");
    auto row = rs.value().next();
    auto lob = row->lob(0);
    EXPECT_EQ(read_all(lob.value(), 4), "12345678");  // 8 bytes, 4-byte buffer
}

TEST(Streaming, FetchErrorPoisonsAndSetsError) {
    Fixture f;
    f.drv.resultSets.push_back({{"doc"}, {{Value{std::string{"x"}}}}});
    halcyon::Error dead;
    dead.code = ErrorCode::Connection;
    f.drv.fetchNextErrors.push_back(dead);
    auto rs = f.conn->queryStreaming("SELECT doc FROM t");
    ASSERT_TRUE(rs.ok());
    auto row = rs.value().next();
    EXPECT_FALSE(row.has_value());
    EXPECT_FALSE(rs.value().ok());
    EXPECT_EQ(rs.value().error()->code, ErrorCode::Connection);
}

TEST(Streaming, AbandonMidStreamClosesCursorOnCachedStatement) {
    Fixture f(/*cacheSize=*/8);
    f.drv.resultSets.push_back({{"doc"}, {{Value{std::string{"abcdefgh"}}}}});
    {
        auto rs = f.conn->queryStreaming("SELECT doc FROM t");
        ASSERT_TRUE(rs.ok());
        auto row = rs.value().next();
        auto lob = row->lob(0);
        std::byte buf[2];
        ASSERT_TRUE(lob.value().read(buf, sizeof(buf)).ok());  // partial read
    }  // StreamingResultSet destroyed mid-stream
    EXPECT_GE(f.drv.closeCursorCalls, 1);  // lease release reset the cursor
}

TEST(Streaming, TransactionForwardsQueryStreaming) {
    Fixture f;
    f.drv.resultSets.push_back({{"n"}, {{Value{std::int64_t{5}}}}});
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto rs = tx.value().queryStreaming("SELECT n FROM t");
    ASSERT_TRUE(rs.ok());
    auto row = rs.value().next();
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->get<std::int64_t>(0).value(), 5);
    ASSERT_TRUE(tx.value().commit().ok());
}

TEST(LobReaderSinks, ToStringAndToVector) {
    Fixture f;
    f.drv.resultSets.push_back(
        {{"a", "b"},
         {{Value{std::string{"text cell"}}, Value{std::string{"bin cell"}}}}});
    f.drv.lobChunkCap = 3;
    auto rs = f.conn->queryStreaming("SELECT a, b FROM t");
    auto row = rs.value().next();
    auto s = row->lob(0).value().toString();
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(s.value(), "text cell");
    auto v = row->lob(1).value().toVector();
    ASSERT_TRUE(v.ok());
    EXPECT_EQ(v.value().size(), 8u);
}

TEST(LobReaderSinks, ToStreamAndToFile) {
    Fixture f;
    f.drv.resultSets.push_back(
        {{"a", "b"},
         {{Value{std::string{"stream me"}}, Value{std::string{"file me"}}}}});
    auto rs = f.conn->queryStreaming("SELECT a, b FROM t");
    auto row = rs.value().next();
    std::ostringstream out;
    ASSERT_TRUE(row->lob(0).value().toStream(out).ok());
    EXPECT_EQ(out.str(), "stream me");
    const std::string path = ::testing::TempDir() + "halcyon_lob_sink.bin";
    ASSERT_TRUE(row->lob(1).value().toFile(path).ok());
    std::ifstream in(path, std::ios::binary);
    std::string back((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    EXPECT_EQ(back, "file me");
}

TEST(LobReaderSinks, NullLobYieldsEmpty) {
    Fixture f;
    f.drv.resultSets.push_back({{"a"}, {{Value{Null{}}}}});
    auto rs = f.conn->queryStreaming("SELECT a FROM t");
    auto row = rs.value().next();
    auto lob = row->lob(0);
    ASSERT_TRUE(lob.ok());
    auto s = lob.value().toString();
    ASSERT_TRUE(s.ok());
    EXPECT_TRUE(s.value().empty());
    EXPECT_TRUE(lob.value().isNull());
}

TEST(DatabaseStreaming, RowsStreamAndLeaseReturnsToPool) {
    MockCliDriver drv;
    halcyon::PoolConfig cfg;
    cfg.min = 1;
    cfg.max = 1;
    cfg.startMaintenanceThread = false;
    auto db = halcyon::Database::open(drv, "dsn", cfg);
    ASSERT_TRUE(db.ok());
    drv.resultSets.push_back(
        {{"id", "doc"},
         {{Value{std::int64_t{1}}, Value{std::string{"payload"}}}}});
    {
        auto rs = db.value().queryStreaming("SELECT id, doc FROM docs");
        ASSERT_TRUE(rs.ok()) << rs.error().message;
        auto row = rs.value().next();
        ASSERT_TRUE(row.has_value());
        EXPECT_EQ(row->get<std::int64_t>(0).value(), 1);
        auto lob = row->lob(1);
        ASSERT_TRUE(lob.ok());
        EXPECT_EQ(lob.value().toString().value(), "payload");
        EXPECT_EQ(db.value().pool().idle_count(), 0u);  // lease held
    }
    EXPECT_EQ(db.value().pool().idle_count(), 1u);  // lease returned
}

// The move-assignment must close the old cursor BEFORE returning the old
// connection to the pool. A single-threaded functional check cannot see the
// ordering (the broken default also eventually closes the cursor), so these
// tests record the exact interleaving: with the fix, "close"/"rollback" precede
// the pool "release"; the broken default memberwise assignment reverses them.
TEST(DatabaseStreaming, MoveAssignClosesCursorBeforeLeaseReturns) {
    MockCliDriver drv;
    std::vector<std::string> order;
    halcyon::PoolConfig cfg;
    cfg.min = 2;
    cfg.max = 2;
    cfg.startMaintenanceThread = false;
    cfg.statementCacheSize = 8;  // cached lease -> closeCursor on release
    cfg.observability.metrics = std::make_shared<OrderSink>(&order);
    auto db = halcyon::Database::open(drv, "dsn", cfg);
    ASSERT_TRUE(db.ok());
    drv.resultSets.push_back({{"doc"}, {{Value{std::string{"a"}}}}});
    drv.resultSets.push_back({{"doc"}, {{Value{std::string{"b"}}}}});
    auto a = db.value().queryStreaming("SELECT doc FROM t").value();
    ASSERT_TRUE(a.next().has_value());
    auto b = db.value().queryStreaming("SELECT doc FROM t").value();
    ASSERT_TRUE(b.next().has_value());

    drv.onCloseCursor = [&] { order.push_back("close"); };
    order.clear();
    a = std::move(b);
    drv.onCloseCursor = nullptr;

    auto firstClose = std::find(order.begin(), order.end(), "close");
    auto firstRelease = std::find(order.begin(), order.end(), "release");
    ASSERT_NE(firstClose, order.end());    // old cursor was closed
    ASSERT_NE(firstRelease, order.end());  // old connection returned
    EXPECT_LT(firstClose, firstRelease);   // closed BEFORE the connection returned
}

TEST(DatabaseQuery, MoveAssignClosesCursorBeforeLeaseReturns) {
    MockCliDriver drv;
    std::vector<std::string> order;
    halcyon::PoolConfig cfg;
    cfg.min = 2;
    cfg.max = 2;
    cfg.startMaintenanceThread = false;
    cfg.statementCacheSize = 8;
    cfg.observability.metrics = std::make_shared<OrderSink>(&order);
    auto db = halcyon::Database::open(drv, "dsn", cfg);
    ASSERT_TRUE(db.ok());
    drv.resultSets.push_back({{"n"}, {{Value{std::int64_t{1}}}}});
    drv.resultSets.push_back({{"n"}, {{Value{std::int64_t{2}}}}});
    auto a = db.value().query("SELECT n FROM t").value();
    auto b = db.value().query("SELECT n FROM t").value();

    drv.onCloseCursor = [&] { order.push_back("close"); };
    order.clear();
    a = std::move(b);
    drv.onCloseCursor = nullptr;

    auto firstClose = std::find(order.begin(), order.end(), "close");
    auto firstRelease = std::find(order.begin(), order.end(), "release");
    ASSERT_NE(firstClose, order.end());
    ASSERT_NE(firstRelease, order.end());
    EXPECT_LT(firstClose, firstRelease);
}

TEST(DatabaseScopedTxn, MoveAssignEndsTransactionBeforeLeaseReturns) {
    MockCliDriver drv;
    std::vector<std::string> order;
    halcyon::PoolConfig cfg;
    cfg.min = 2;
    cfg.max = 2;
    cfg.startMaintenanceThread = false;
    cfg.observability.metrics = std::make_shared<OrderSink>(&order);
    auto db = halcyon::Database::open(drv, "dsn", cfg);
    ASSERT_TRUE(db.ok());
    auto a = db.value().begin().value();  // active transaction on conn 1
    auto b = db.value().begin().value();  // active transaction on conn 2

    drv.onRollback = [&] { order.push_back("rollback"); };
    order.clear();
    a = std::move(b);  // old a's transaction must roll back before its lease returns
    drv.onRollback = nullptr;

    auto firstRollback = std::find(order.begin(), order.end(), "rollback");
    auto firstRelease = std::find(order.begin(), order.end(), "release");
    ASSERT_NE(firstRollback, order.end());  // old transaction was rolled back
    ASSERT_NE(firstRelease, order.end());
    EXPECT_LT(firstRollback, firstRelease);  // ended BEFORE the connection returned
}

TEST(DatabaseStreaming, ConnectionErrorDiscardsLease) {
    MockCliDriver drv;
    halcyon::PoolConfig cfg;
    cfg.min = 1;
    cfg.max = 1;
    cfg.startMaintenanceThread = false;
    auto db = halcyon::Database::open(drv, "dsn", cfg);
    ASSERT_TRUE(db.ok());
    drv.resultSets.push_back({{"doc"}, {{Value{std::string{"x"}}}}});
    halcyon::Error dead;
    dead.code = ErrorCode::Connection;
    drv.fetchNextErrors.push_back(dead);
    const int disconnectsBefore = drv.disconnectCalls;
    {
        auto rs = db.value().queryStreaming("SELECT doc FROM docs");
        ASSERT_TRUE(rs.ok());
        EXPECT_FALSE(rs.value().next().has_value());
        EXPECT_FALSE(rs.value().ok());
    }
    EXPECT_GT(drv.disconnectCalls, disconnectsBefore);  // broken lease discarded
}
