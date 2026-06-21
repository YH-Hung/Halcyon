#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "halcyon/halcyon.hpp"

using halcyon::Connection;
using halcyon::params;

namespace {
std::optional<std::string> dsn() {
    if (const char* v = std::getenv("HALCYON_TEST_DSN")) return std::string(v);
    return std::nullopt;
}
}  // namespace

struct Person {
    int id;
    std::string name;
    std::optional<int> age;
};
HALCYON_REFLECT(Person, id, name, age);

struct BatchCount {
    std::int64_t c;
};
HALCYON_REFLECT(BatchCount, c);

class Db2Integration : public ::testing::Test {
protected:
    void SetUp() override {
        auto d = dsn();
        if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
        driver_ = halcyon::detail::cli::make_db2_cli_driver();
        auto c = Connection::open(*driver_, {*d});
        ASSERT_TRUE(c.ok()) << c.error().message;
        conn_ = std::make_unique<Connection>(std::move(c.value()));
        conn_->execute("DROP TABLE halcyon_people");  // ignore error if absent
        ASSERT_TRUE(
            conn_->execute("CREATE TABLE halcyon_people("
                           "id INT NOT NULL, name VARCHAR(64), age INT)")
                .ok());
    }

    std::unique_ptr<halcyon::detail::cli::ICliDriver> driver_;
    std::unique_ptr<Connection> conn_;
};

TEST_F(Db2Integration, InsertSelectStructMapping) {
    ASSERT_EQ(conn_->execute("INSERT INTO halcyon_people VALUES (?, ?, ?)", 1,
                             std::string{"ada"}, 36)
                  .value(),
              1);
    ASSERT_TRUE(conn_
                    ->execute("INSERT INTO halcyon_people(id, name) VALUES "
                              "(:id, :name)",
                              params{{"id", 2}, {"name", std::string{"bob"}}})
                    .ok());

    auto people =
        conn_->queryAs<Person>("SELECT id, name, age FROM halcyon_people ORDER BY id");
    ASSERT_TRUE(people.ok()) << people.error().message;
    ASSERT_EQ(people.value().size(), 2u);
    EXPECT_EQ(people.value()[0].name, "ada");
    EXPECT_EQ(*people.value()[0].age, 36);
    EXPECT_FALSE(people.value()[1].age.has_value());
}

TEST_F(Db2Integration, TupleIterationAndAnonymousParams) {
    ASSERT_TRUE(conn_->execute("INSERT INTO halcyon_people VALUES (?,?,?)", 10,
                               std::string{"x"}, 1).ok());
    auto rs = conn_->query("SELECT id, name FROM halcyon_people WHERE id >= ?", 10);
    ASSERT_TRUE(rs.ok()) << rs.error().message;
    int count = 0;
    for (auto& row : rs.value()) {
        auto [id, name] = row.as<int, std::string>();
        EXPECT_GE(id, 10);
        (void)name;
        ++count;
    }
    EXPECT_GE(count, 1);
}

#include <cstdint>
#include <future>
#include <thread>
#include <vector>

TEST(Db2PoolIntegration, ConcurrentAcquireAndQuery) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";

    auto driver = halcyon::detail::cli::make_db2_cli_driver();
    halcyon::PoolConfig cfg;
    cfg.min = 2;
    cfg.max = 6;
    cfg.validateOnAcquire = true;
    auto pool = halcyon::ConnectionPool::create(*driver, {*d}, cfg);
    ASSERT_TRUE(pool.ok()) << pool.error().message;

    halcyon::Executor ex(6);
    std::vector<std::future<halcyon::Result<std::int64_t>>> futs;
    for (int i = 0; i < 24; ++i) {
        futs.push_back(halcyon::async_with_connection(
            ex, *pool.value(),
            [](halcyon::Connection& c) -> halcyon::Result<std::int64_t> {
                auto rs = c.query("SELECT 1 FROM SYSIBM.SYSDUMMY1");
                if (!rs.ok()) return rs.error();
                std::int64_t n = 0;
                for (auto& row : rs.value()) n += std::get<0>(row.as<std::int64_t>());
                return n;
            }));
    }
    for (auto& f : futs) {
        auto r = f.get();
        ASSERT_TRUE(r.ok()) << r.error().message;
        EXPECT_EQ(r.value(), 1);
    }
}

// Exercises the §7 type-mapping completion against live Db2: real binary reads
// (BLOB with embedded NUL and > one read-buffer in size), exact Decimal, and the
// DATE/TIME/TIMESTAMP wrappers — including binding string-carried values INTO
// strongly-typed temporal/decimal columns (the implicit SQL_C_CHAR cast).
TEST(Db2TypeMapping, BinaryDecimalTemporalRoundTrip) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";

    auto db = halcyon::Database::open(*d);
    ASSERT_TRUE(db.ok()) << db.error().message;
    auto& h = db.value();

    h.execute("DROP TABLE halcyon_types");  // ignore error if absent
    ASSERT_TRUE(h.execute("CREATE TABLE halcyon_types("
                          "id INT NOT NULL, payload BLOB(1M), "
                          "amount DECIMAL(31,5), d DATE, t TIME, ts TIMESTAMP)")
                    .ok());

    std::vector<std::byte> blob(5000, std::byte{0xAB});  // > 4096-byte read chunk
    blob[10] = std::byte{0x00};                          // embedded NULs
    blob[4096] = std::byte{0x00};

    auto ins = h.execute(
        "INSERT INTO halcyon_types(id, payload, amount, d, t, ts) "
        "VALUES (?, ?, ?, ?, ?, ?)",
        1, blob, halcyon::Decimal{"12345.67800"}, halcyon::Date{"2026-06-21"},
        halcyon::Time{"13:45:00"}, halcyon::Timestamp{"2026-06-21 13:45:00.123456"});
    ASSERT_TRUE(ins.ok()) << ins.error().message;  // string→temporal/decimal bind cast

    auto qr = h.query(
        "SELECT payload, amount, d, t, ts FROM halcyon_types WHERE id = ?", 1);
    ASSERT_TRUE(qr.ok()) << qr.error().message;
    int rows = 0;
    for (auto& row : qr.value()) {
        auto [payload, amount, date, time, ts] =
            row.as<std::vector<std::byte>, halcyon::Decimal, halcyon::Date,
                   halcyon::Time, halcyon::Timestamp>();
        EXPECT_EQ(payload, blob);  // byte-exact: the binary-read fix
        EXPECT_EQ(amount, halcyon::Decimal{"12345.67800"});  // exact scale
        EXPECT_EQ(date, halcyon::Date{"2026-06-21"});
        EXPECT_EQ(time.value, "13:45:00");
        // Db2's timestamp text form varies (space vs '-' separators); assert the
        // components survive rather than an exact string.
        EXPECT_NE(ts.value.find("2026-06-21"), std::string::npos);
        EXPECT_NE(ts.value.find("123456"), std::string::npos);
        ++rows;
    }
    EXPECT_TRUE(qr.value().ok()) << "iteration ended on a fetch error";
    EXPECT_EQ(rows, 1);

    // std::chrono interop (spec §7): bind a system_clock::time_point INTO the
    // TIMESTAMP column and read it back as a time_point, asserting exact equality
    // (exercises the chrono formatter, Db2's implicit cast, and the parser).
    using TP = std::chrono::system_clock::time_point;
    const TP tp_in =
        halcyon::TypeBinder<TP>::from_value(
            halcyon::detail::cli::Value{std::string{"2025-01-02 03:04:05.000000"}})
            .value();
    ASSERT_TRUE(
        h.execute("INSERT INTO halcyon_types(id, ts) VALUES (?, ?)", 2, tp_in)
            .ok());
    auto qr2 = h.query("SELECT ts FROM halcyon_types WHERE id = ?", 2);
    ASSERT_TRUE(qr2.ok()) << qr2.error().message;
    int chrono_rows = 0;
    for (auto& row : qr2.value()) {
        auto [tp_out] = row.as<TP>();
        EXPECT_EQ(tp_out, tp_in);  // round-trips through a real Db2 TIMESTAMP
        ++chrono_rows;
    }
    EXPECT_EQ(chrono_rows, 1);

    h.execute("DROP TABLE halcyon_types");
}

TEST(Db2FacadeIntegration, QueryExecuteAndTransaction) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";

    auto driver = halcyon::detail::cli::make_db2_cli_driver();
    auto db = halcyon::Database::open(*driver, *d);
    ASSERT_TRUE(db.ok()) << db.error().message;

    auto qr = db.value().query("SELECT 1 FROM SYSIBM.SYSDUMMY1");
    ASSERT_TRUE(qr.ok()) << qr.error().message;
    std::int64_t sum = 0;
    for (auto& row : qr.value()) sum += std::get<0>(row.as<std::int64_t>());
    EXPECT_EQ(sum, 1);

    auto txr = halcyon::transaction(
        db.value(),
        [](halcyon::Transaction& tx) -> halcyon::Result<std::int64_t> {
            return tx.execute("SELECT 1 FROM SYSIBM.SYSDUMMY1");
        });
    ASSERT_TRUE(txr.ok()) << txr.error().message;
}

// Exercises the ergonomic surface end to end: driver-less Database::open(dsn)
// (the facade owns the CLI driver), tuple-row batchOf, and queryAsync<T>.
TEST(Db2FacadeIntegration, ErgonomicOpenBatchAndAsync) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";

    auto db = halcyon::Database::open(*d);  // no CLI types at the call site
    ASSERT_TRUE(db.ok()) << db.error().message;
    auto& h = db.value();

    h.execute("DROP TABLE halcyon_batch");  // ignore error if absent
    ASSERT_TRUE(
        h.execute("CREATE TABLE halcyon_batch(a INT NOT NULL, b VARCHAR(8))").ok());

    auto batch = halcyon::batchOf({
        std::make_tuple(std::int64_t{1}, std::string{"x"}),
        std::make_tuple(std::int64_t{2}, std::string{"y"}),
    });
    auto inserted =
        h.executeBatch("INSERT INTO halcyon_batch(a,b) VALUES (?,?)", batch);
    ASSERT_TRUE(inserted.ok()) << inserted.error().message;
    EXPECT_EQ(inserted.value(), 2);

    auto f = h.queryAsync<BatchCount>("SELECT COUNT(*) FROM halcyon_batch");
    auto r = f.get();
    ASSERT_TRUE(r.ok()) << r.error().message;
    ASSERT_EQ(r.value().size(), 1u);
    EXPECT_EQ(r.value()[0].c, 2);

    h.execute("DROP TABLE halcyon_batch");
}
