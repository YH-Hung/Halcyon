#include <gtest/gtest.h>

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
