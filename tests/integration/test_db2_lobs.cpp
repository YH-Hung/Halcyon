#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "halcyon/halcyon.hpp"

using halcyon::Connection;

namespace {

std::optional<std::string> dsn() {
    if (const char* v = std::getenv("HALCYON_TEST_DSN")) return std::string(v);
    return std::nullopt;
}

// FNV-1a over a byte range — cheap content checksum for round-trip asserts.
std::uint64_t fnv1a(const std::byte* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

std::vector<std::byte> random_bytes(std::size_t n, std::uint32_t seed) {
    std::vector<std::byte> out(n);
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < n; i += 4) {
        const std::uint32_t r = rng();
        std::memcpy(out.data() + i, &r, std::min<std::size_t>(4, n - i));
    }
    return out;
}

constexpr std::size_t kBigLob = 32u * 1024u * 1024u;  // 32 MiB (spec §8)

}  // namespace

// queryAs<T> requires a reflected struct (tuples are for Row::as<>).
struct LobCountRow {
    std::int64_t c;
};
HALCYON_REFLECT(LobCountRow, c);

class Db2Lobs : public ::testing::Test {
protected:
    void SetUp() override {
        auto d = dsn();
        if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
        driver_ = halcyon::detail::cli::make_db2_cli_driver();
        auto c = Connection::open(*driver_, {*d});
        ASSERT_TRUE(c.ok()) << c.error().message;
        conn_ = std::make_unique<Connection>(std::move(c.value()));
        conn_->execute("DROP TABLE halcyon_v11_lobs");  // ignore error if absent
        ASSERT_TRUE(conn_->execute("CREATE TABLE halcyon_v11_lobs("
                                   "id INT NOT NULL, doc BLOB(64M), note CLOB(2M))")
                        .ok());
    }

    std::unique_ptr<halcyon::detail::cli::ICliDriver> driver_;
    std::unique_ptr<Connection> conn_;
};

TEST_F(Db2Lobs, Blob32MiBFileRoundTrip) {
    const auto payload = random_bytes(kBigLob, 42);
    const std::uint64_t sum = fnv1a(payload.data(), payload.size());
    const std::string inPath = ::testing::TempDir() + "halcyon_lob_in.bin";
    const std::string outPath = ::testing::TempDir() + "halcyon_lob_out.bin";
    {
        std::ofstream f(inPath, std::ios::binary);
        f.write(reinterpret_cast<const char*>(payload.data()),
                static_cast<std::streamsize>(payload.size()));
    }
    auto ins = conn_->execute(
        "INSERT INTO halcyon_v11_lobs(id, doc) VALUES (?, ?)", 1,
        halcyon::lobFile(inPath));
    ASSERT_TRUE(ins.ok()) << ins.error().message;

    auto rs = conn_->queryStreaming(
        "SELECT doc FROM halcyon_v11_lobs WHERE id = ?", 1);
    ASSERT_TRUE(rs.ok()) << rs.error().message;
    auto row = rs.value().next();
    ASSERT_TRUE(row.has_value());
    auto lob = row->lob(0);
    ASSERT_TRUE(lob.ok());
    ASSERT_TRUE(lob.value().toFile(outPath).ok());
    EXPECT_TRUE(rs.value().ok());

    std::ifstream back(outPath, std::ios::binary);
    std::vector<char> got((std::istreambuf_iterator<char>(back)),
                          std::istreambuf_iterator<char>());
    ASSERT_EQ(got.size(), payload.size());
    EXPECT_EQ(fnv1a(reinterpret_cast<const std::byte*>(got.data()), got.size()),
              sum);
}

TEST_F(Db2Lobs, StreamAndCallbackSourcesRoundTrip) {
    // ≥32 MiB through both stream and callback sources (plan Task 19 / spec §8).
    const auto payload = random_bytes(kBigLob, 7);
    const std::uint64_t sum = fnv1a(payload.data(), payload.size());

    std::string asString(reinterpret_cast<const char*>(payload.data()),
                         payload.size());
    std::istringstream in(asString);
    ASSERT_TRUE(conn_
                    ->execute("INSERT INTO halcyon_v11_lobs(id, doc) VALUES (?, ?)",
                              2, halcyon::lobStream(in, payload.size()))
                    .ok());

    std::size_t off = 0;
    ASSERT_TRUE(conn_
                    ->execute("INSERT INTO halcyon_v11_lobs(id, doc) VALUES (?, ?)",
                              3,
                              halcyon::lobCallback(
                                  [&](std::byte* buf, std::size_t cap) {
                                      const std::size_t n = std::min(
                                          cap, payload.size() - off);
                                      std::memcpy(buf, payload.data() + off, n);
                                      off += n;
                                      return n;
                                  },
                                  payload.size()))
                    .ok());

    for (int id : {2, 3}) {
        auto rs = conn_->queryStreaming(
            "SELECT doc FROM halcyon_v11_lobs WHERE id = ?", id);
        ASSERT_TRUE(rs.ok());
        auto row = rs.value().next();
        ASSERT_TRUE(row.has_value());
        auto bytes = row->lob(0).value().toVector();
        ASSERT_TRUE(bytes.ok());
        ASSERT_EQ(bytes.value().size(), payload.size()) << "id=" << id;
        EXPECT_EQ(fnv1a(bytes.value().data(), bytes.value().size()), sum);
    }
}

TEST_F(Db2Lobs, ClobRoundTrip) {
    std::string text;
    text.reserve(1u << 20);
    while (text.size() < (1u << 20)) text += "halcyon clob payload line\n";
    std::istringstream in(text);
    ASSERT_TRUE(conn_
                    ->execute("INSERT INTO halcyon_v11_lobs(id, note) VALUES (?, ?)",
                              4, halcyon::lobStream(in, text.size()).asClob())
                    .ok());
    auto rs = conn_->queryStreaming(
        "SELECT note FROM halcyon_v11_lobs WHERE id = ?", 4);
    ASSERT_TRUE(rs.ok());
    auto row = rs.value().next();
    ASSERT_TRUE(row.has_value());
    auto got = row->lob(0).value().toString();
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.value(), text);
}

TEST_F(Db2Lobs, EmptyAndNullLobs) {
    ASSERT_TRUE(conn_
                    ->execute("INSERT INTO halcyon_v11_lobs(id, doc) VALUES (?, ?)",
                              5,
                              halcyon::lobCallback(
                                  [](std::byte*, std::size_t) -> std::size_t {
                                      return 0;  // empty LOB
                                  }))
                    .ok());
    // SPIKE FINDING (Task 19): binding a SQL NULL to a LOB column via the
    // ordinary typed-parameter path (std::optional{}) fails with SQL0301N /
    // SQLSTATE 07006 — the untyped NULL binds as VARCHAR, which Db2 rejects for
    // a BLOB column. This is pre-existing base-library binding behavior, not a
    // streaming feature. A NULL LOB is created here via a SQL literal instead;
    // the streaming read path (getDataChunk -> isNull) is what this test
    // exercises. (Recorded in the v1.1 design spec §9.)
    ASSERT_TRUE(
        conn_->execute("INSERT INTO halcyon_v11_lobs(id, doc) VALUES (6, NULL)")
            .ok());

    auto rs = conn_->queryStreaming(
        "SELECT id, doc FROM halcyon_v11_lobs WHERE id IN (5, 6) ORDER BY id");
    ASSERT_TRUE(rs.ok());
    auto row5 = rs.value().next();
    ASSERT_TRUE(row5.has_value());
    EXPECT_EQ(row5->get<std::int64_t>(0).value(), 5);
    auto empty = row5->lob(1).value();
    auto emptyBytes = empty.toVector();
    ASSERT_TRUE(emptyBytes.ok());
    EXPECT_TRUE(emptyBytes.value().empty());
    EXPECT_FALSE(empty.isNull());

    auto row6 = rs.value().next();
    ASSERT_TRUE(row6.has_value());
    EXPECT_EQ(row6->get<std::int64_t>(0).value(), 6);
    auto nul = row6->lob(1).value();
    auto nulBytes = nul.toVector();
    ASSERT_TRUE(nulBytes.ok());
    EXPECT_TRUE(nulBytes.value().empty());
    EXPECT_TRUE(nul.isNull());
}

TEST_F(Db2Lobs, MixedRowAscendingReads) {
    std::istringstream doc("binary part");
    ASSERT_TRUE(
        conn_
            ->execute(
                "INSERT INTO halcyon_v11_lobs(id, doc, note) VALUES (?, ?, ?)",
                7, halcyon::lobStream(doc),
                std::string{"note part"})
            .ok());
    auto rs = conn_->queryStreaming(
        "SELECT id, note, doc FROM halcyon_v11_lobs WHERE id = ?", 7);
    ASSERT_TRUE(rs.ok());
    auto row = rs.value().next();
    ASSERT_TRUE(row.has_value());
    EXPECT_EQ(row->get<std::int64_t>(0).value(), 7);
    EXPECT_EQ(row->lob(1).value().toString().value(), "note part");
    EXPECT_EQ(row->lob(2).value().toString().value(), "binary part");
}

TEST_F(Db2Lobs, ThrowingSourceIsMappingErrorAndConnectionReusable) {
    // A throwing LOB callback must not escape the seam or leave the statement in
    // SQL_NEED_DATA state: the driver cancels and maps to a Mapping error, and
    // the connection stays usable.
    auto r = conn_->execute(
        "INSERT INTO halcyon_v11_lobs(id, doc) VALUES (?, ?)", 99,
        halcyon::lobCallback([](std::byte*, std::size_t) -> std::size_t {
            throw std::runtime_error("source blew up");
        }));
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, halcyon::ErrorCode::Mapping);
    auto rows =
        conn_->queryAs<LobCountRow>("SELECT COUNT(*) FROM halcyon_v11_lobs");
    ASSERT_TRUE(rows.ok()) << rows.error().message;  // connection still usable
}

TEST_F(Db2Lobs, AbandonMidStreamThenReuseConnection) {
    const auto payload = random_bytes(2u * 1024u * 1024u, 11);
    std::string asString(reinterpret_cast<const char*>(payload.data()),
                         payload.size());
    std::istringstream in(asString);
    ASSERT_TRUE(conn_
                    ->execute("INSERT INTO halcyon_v11_lobs(id, doc) VALUES (?, ?)",
                              8, halcyon::lobStream(in))
                    .ok());
    {
        auto rs = conn_->queryStreaming(
            "SELECT doc FROM halcyon_v11_lobs WHERE id = ?", 8);
        ASSERT_TRUE(rs.ok());
        auto row = rs.value().next();
        ASSERT_TRUE(row.has_value());
        auto lob = row->lob(0);
        ASSERT_TRUE(lob.ok());
        std::byte buf[1024];
        ASSERT_TRUE(lob.value().read(buf, sizeof(buf)).ok());  // partial
    }  // abandoned mid-stream
    auto rows =
        conn_->queryAs<LobCountRow>("SELECT COUNT(*) FROM halcyon_v11_lobs");
    ASSERT_TRUE(rows.ok()) << rows.error().message;  // connection still usable
    EXPECT_GE(rows.value().at(0).c, 1);
}
