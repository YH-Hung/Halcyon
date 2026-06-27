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
                               std::string{"x"}, 1)
                    .ok());
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
        EXPECT_EQ(payload, blob);                            // byte-exact: the binary-read fix
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

// Exercises the §7 bool mapping against live Db2: a bool parameter binds as
// SQL_C_BIT / SQL_BIT and round-trips through a SMALLINT column.
TEST(Db2BoolMapping, BoolParameterBindsAsBitAndRoundTrips) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";

    auto db = halcyon::Database::open(*d);
    ASSERT_TRUE(db.ok()) << db.error().message;
    auto& h = db.value();

    h.execute("DROP TABLE halcyon_bool");  // ignore error if absent
    ASSERT_TRUE(
        h.execute("CREATE TABLE halcyon_bool(id INT NOT NULL, flag SMALLINT)").ok());

    ASSERT_TRUE(h.execute("INSERT INTO halcyon_bool VALUES (?, ?)", 1, true).ok());
    ASSERT_TRUE(h.execute("INSERT INTO halcyon_bool VALUES (?, ?)", 2, false).ok());

    auto qr = h.query("SELECT flag FROM halcyon_bool ORDER BY id");
    ASSERT_TRUE(qr.ok()) << qr.error().message;
    std::vector<bool> flags;
    for (auto& row : qr.value()) flags.push_back(std::get<0>(row.as<bool>()));
    EXPECT_TRUE(qr.value().ok()) << "iteration ended on a fetch error";
    ASSERT_EQ(flags.size(), 2u);
    EXPECT_TRUE(flags[0]);
    EXPECT_FALSE(flags[1]);

    h.execute("DROP TABLE halcyon_bool");
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

// Exercises the real closeCursor-based reuse path the mock cannot: with a single
// reused physical connection and a warm statement cache, the SAME SELECT run
// twice with different params must reuse the cached prepared statement and still
// return the correct rows. Each query is scoped so its result set (and thus the
// cache lease) is released before the next acquire — only then does the second
// call hit the cached statement and re-execute it after closing the first cursor.
TEST(Db2CacheIntegration, CachedStatementReuseReturnsCorrectRows) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";

    halcyon::PoolConfig cfg;
    cfg.min = 1;
    cfg.max = 1;  // force a single reused physical connection
    cfg.statementCacheSize = 8;
    auto db = halcyon::Database::open(*d, cfg);
    ASSERT_TRUE(db.ok()) << db.error().message;
    auto& h = db.value();

    const std::string sql = "SELECT CAST(? AS INTEGER) FROM SYSIBM.SYSDUMMY1";

    {
        auto qr = h.query(sql, 1);
        ASSERT_TRUE(qr.ok()) << qr.error().message;
        int got = 0, rows = 0;
        for (auto& row : qr.value()) {
            got = std::get<0>(row.as<int>());
            ++rows;
        }
        EXPECT_TRUE(qr.value().ok()) << "iteration ended on a fetch error";
        EXPECT_EQ(rows, 1);
        EXPECT_EQ(got, 1);
    }  // qr destroyed -> lease returned to cache, first cursor closed
    {
        auto qr = h.query(sql, 2);  // re-binds + re-executes the cached statement
        ASSERT_TRUE(qr.ok()) << qr.error().message;
        int got = 0, rows = 0;
        for (auto& row : qr.value()) {
            got = std::get<0>(row.as<int>());
            ++rows;
        }
        EXPECT_TRUE(qr.value().ok()) << "iteration ended on a fetch error";
        EXPECT_EQ(rows, 1);
        EXPECT_EQ(got, 2);  // correct rows from the reused cursor
    }
}

// --- True array binding (Task 2) ---

namespace {
struct ArrRow {
    std::int64_t id;
    std::string name;
    std::optional<std::int64_t> qty;  // nullable column
};
}  // namespace
HALCYON_REFLECT(ArrRow, id, name, qty);

TEST(Db2ArrayBinding, MultiRowInsertAggregatesCountAndNulls) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
    auto dbh = halcyon::Database::open(*d);
    ASSERT_TRUE(dbh.ok()) << dbh.error().message;
    auto& h = dbh.value();
    h.execute("DROP TABLE halcyon_arr");  // ignore if absent
    ASSERT_TRUE(h.execute(
                     "CREATE TABLE halcyon_arr(id BIGINT NOT NULL, "
                     "name VARCHAR(32), qty BIGINT)")
                    .ok());

    std::vector<ArrRow> rows = {
        {1, "a", 10},
        {2, "b", std::nullopt},
        {3, "c", 30},
        {4, "d", 40},
        {5, "e", std::nullopt},
    };
    auto n = h.executeBatch(
        "INSERT INTO halcyon_arr(id,name,qty) VALUES (?,?,?)",
        halcyon::batchOf(rows));
    ASSERT_TRUE(n.ok()) << n.error().message;
    EXPECT_EQ(n.value(), 5);

    auto count = h.queryAsOrThrow<BatchCount>(
        "SELECT COUNT(*) AS c FROM halcyon_arr");
    ASSERT_EQ(count.size(), 1u);
    EXPECT_EQ(count[0].c, 5);

    auto nulls = h.queryAsOrThrow<BatchCount>(
        "SELECT COUNT(*) AS c FROM halcyon_arr WHERE qty IS NULL");
    EXPECT_EQ(nulls[0].c, 2);

    // Value fidelity: every array-bound column round-trips exactly, not just the
    // row/NULL counts. Distinct per-row int64 ids, strings, and interleaved NULL
    // quantities catch column-wise stride, byte-order, and indicator-alignment
    // bugs that an aggregate count would miss.
    auto back = h.queryAsOrThrow<ArrRow>(
        "SELECT id, name, qty FROM halcyon_arr ORDER BY id");
    ASSERT_EQ(back.size(), 5u);
    EXPECT_EQ(back[0].id, 1);
    EXPECT_EQ(back[0].name, "a");
    ASSERT_TRUE(back[0].qty.has_value());
    EXPECT_EQ(*back[0].qty, 10);
    EXPECT_EQ(back[1].id, 2);
    EXPECT_EQ(back[1].name, "b");
    EXPECT_FALSE(back[1].qty.has_value());
    EXPECT_EQ(back[2].id, 3);
    EXPECT_EQ(back[2].name, "c");
    ASSERT_TRUE(back[2].qty.has_value());
    EXPECT_EQ(*back[2].qty, 30);
    EXPECT_EQ(back[4].id, 5);
    EXPECT_EQ(back[4].name, "e");
    EXPECT_FALSE(back[4].qty.has_value());

    h.execute("DROP TABLE halcyon_arr");
}

TEST(Db2ArrayBinding, ConstraintViolationIsClassifiedAndChunkAtomic) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
    auto dbh = halcyon::Database::open(*d);
    ASSERT_TRUE(dbh.ok()) << dbh.error().message;
    auto& h = dbh.value();
    h.execute("DROP TABLE halcyon_arr_pk");
    ASSERT_TRUE(h.execute(
                     "CREATE TABLE halcyon_arr_pk(id BIGINT NOT NULL PRIMARY KEY, "
                     "name VARCHAR(32))")
                    .ok());

    // The third row duplicates id=1 -> unique-constraint violation. Db2 applies
    // the array chunk atomically, so the whole batch is rolled back and the
    // error is classified from SQLSTATE 23505. (Db2 CLI does not surface which
    // row failed for an array bind, so the contract is a single classified
    // Error, not a per-row index — wrap in a transaction and re-drive.)
    auto batch = halcyon::batchOf({
        std::make_tuple(std::int64_t{1}, std::string{"a"}),
        std::make_tuple(std::int64_t{2}, std::string{"b"}),
        std::make_tuple(std::int64_t{1}, std::string{"dup"}),
    });
    auto n = h.executeBatch(
        "INSERT INTO halcyon_arr_pk(id,name) VALUES (?,?)", batch);
    ASSERT_FALSE(n.ok());
    EXPECT_EQ(n.error().code, halcyon::ErrorCode::Constraint);
    EXPECT_EQ(n.error().sqlstate, "23505");

    // Atomic per chunk: nothing from the failed batch is committed.
    auto count = h.queryAsOrThrow<BatchCount>(
        "SELECT COUNT(*) AS c FROM halcyon_arr_pk");
    ASSERT_EQ(count.size(), 1u);
    EXPECT_EQ(count[0].c, 0);

    h.execute("DROP TABLE halcyon_arr_pk");
}

TEST(Db2ArrayBinding, MultiChunkInsertExceedsByteBudget) {
    auto d = dsn();
    if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
    auto dbh = halcyon::Database::open(*d);
    ASSERT_TRUE(dbh.ok()) << dbh.error().message;
    auto& h = dbh.value();
    h.execute("DROP TABLE halcyon_arr_big");
    ASSERT_TRUE(h.execute(
                     "CREATE TABLE halcyon_arr_big(id BIGINT NOT NULL, "
                     "payload VARCHAR(1100))")
                    .ok());

    // ~20 MB of bound data ( > 16 MiB budget ) forces at least two chunks.
    const std::int64_t kRows = 20000;
    const std::string payload(1000, 'x');
    std::vector<std::tuple<std::int64_t, std::string>> rows;
    rows.reserve(kRows);
    for (std::int64_t i = 0; i < kRows; ++i) rows.emplace_back(i, payload);

    auto n = h.executeBatch(
        "INSERT INTO halcyon_arr_big(id,payload) VALUES (?,?)",
        halcyon::batchOf(rows));
    ASSERT_TRUE(n.ok()) << n.error().message;
    EXPECT_EQ(n.value(), kRows);

    auto count = h.queryAsOrThrow<BatchCount>(
        "SELECT COUNT(*) AS c FROM halcyon_arr_big");
    EXPECT_EQ(count[0].c, kRows);

    h.execute("DROP TABLE halcyon_arr_big");
}
