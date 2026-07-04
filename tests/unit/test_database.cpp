#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "halcyon/database.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Database;
using halcyon::PoolConfig;
using halcyon::testing::MockCliDriver;

namespace {
MockCliDriver::ScriptedRows idName(std::int64_t id, std::string name) {
    return MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{halcyon::detail::cli::Value{id},
          halcyon::detail::cli::Value{std::move(name)}}}};
}
PoolConfig noThread() {
    PoolConfig c;
    c.startMaintenanceThread = false;
    return c;
}
}  // namespace

struct Person {
    std::int64_t id;
    std::string name;
};
HALCYON_REFLECT(Person, id, name);

TEST(Database, OpenWarmsPoolToMin) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 2;
    auto db = Database::open(driver, "DATABASE=X;", cfg);
    ASSERT_TRUE(db.ok()) << db.error().message;
    EXPECT_EQ(driver.connectCalls, 2);
}

TEST(Database, ExecuteReturnsRowCount) {
    MockCliDriver driver;
    driver.execRowCounts.push_back(3);
    auto db = Database::open(driver, "X", noThread()).value();
    auto n = db.execute("UPDATE t SET a=? WHERE id=?", 1, 5);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 3);
}

TEST(Database, QueryIteratesRows) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"},
        {{halcyon::detail::cli::Value{std::int64_t{1}}},
         {halcyon::detail::cli::Value{std::int64_t{2}}},
         {halcyon::detail::cli::Value{std::int64_t{4}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto qr = db.query("SELECT id FROM t WHERE id > ?", 0);
    ASSERT_TRUE(qr.ok());
    std::int64_t sum = 0;
    for (auto& row : qr.value()) sum += std::get<0>(row.as<std::int64_t>());
    EXPECT_EQ(sum, 7);
}

TEST(Database, QueryReleasesLeaseWhenResultDropped) {
    MockCliDriver driver;
    driver.resultSets.push_back(idName(1, "a"));
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    auto db = Database::open(driver, "X", cfg).value();
    {
        auto qr = db.query("SELECT id, name FROM t");
        ASSERT_TRUE(qr.ok());
    }
    // lease returned → a second single-slot query still works
    driver.resultSets.push_back(idName(2, "b"));
    auto qr2 = db.query("SELECT id, name FROM t");
    ASSERT_TRUE(qr2.ok());
}

TEST(Database, QueryAsMapsStructs) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{halcyon::detail::cli::Value{std::int64_t{1}},
          halcyon::detail::cli::Value{std::string{"ann"}}},
         {halcyon::detail::cli::Value{std::int64_t{2}},
          halcyon::detail::cli::Value{std::string{"bob"}}}}});
    auto db = Database::open(driver, "X", noThread()).value();
    auto people = db.queryAs<Person>("SELECT id, name FROM people");
    ASSERT_TRUE(people.ok());
    ASSERT_EQ(people.value().size(), 2u);
    EXPECT_EQ(people.value()[1].name, "bob");
}

TEST(Database, NamedParamsThroughFacade) {
    MockCliDriver driver;
    driver.resultSets.push_back(idName(7, "z"));
    auto db = Database::open(driver, "X", noThread()).value();
    auto qr = db.query("SELECT id, name FROM t WHERE id = :id",
                       halcyon::params{{"id", 7}});
    ASSERT_TRUE(qr.ok());
    int rows = 0;
    for (auto& row : qr.value()) {
        (void)row;
        ++rows;
    }
    EXPECT_EQ(rows, 1);
}

TEST(Database, ThrowingOverloadUnwraps) {
    MockCliDriver driver;
    driver.executeErrors.push_back([] {
        halcyon::Error e;
        e.code = halcyon::ErrorCode::Syntax;
        e.message = "boom";
        return e;
    }());
    auto db = Database::open(driver, "X", noThread()).value();
    EXPECT_THROW(db.executeOrThrow("BAD SQL"), halcyon::Exception);
}

#include <stdexcept>

TEST(DatabaseTxn, CommitsOnSuccessfulLambda) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = db.transaction([](halcyon::Transaction& tx) -> halcyon::Result<int> {
        auto n = tx.execute("INSERT INTO t VALUES (?)", 1);
        if (!n.ok()) return n.error();
        return 99;
    });
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 99);
    EXPECT_EQ(driver.commitCalls, 1);
    EXPECT_EQ(driver.rollbackCalls, 0);
}

TEST(DatabaseTxn, RollsBackWhenLambdaReturnsError) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto r = db.transaction([](halcyon::Transaction& tx) -> halcyon::Result<int> {
        (void)tx;
        halcyon::Error e;
        e.code = halcyon::ErrorCode::Constraint;
        e.message = "dup";
        return e;
    });
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(driver.commitCalls, 0);
    EXPECT_EQ(driver.rollbackCalls, 1);
}

TEST(DatabaseTxn, RollsBackWhenLambdaThrows) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    EXPECT_THROW(
        db.transaction([](halcyon::Transaction&) -> halcyon::Result<int> {
            throw std::runtime_error("boom");
        }),
        std::runtime_error);
    EXPECT_EQ(driver.commitCalls, 0);
    EXPECT_EQ(driver.rollbackCalls, 1);
}

TEST(DatabaseTxn, BeginReturnsUsableTransaction) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();
    auto tx = db.begin();
    ASSERT_TRUE(tx.ok());
    ASSERT_TRUE(tx.value().execute("UPDATE t SET a=?", 1).ok());
    ASSERT_TRUE(tx.value().commit().ok());
    EXPECT_EQ(driver.commitCalls, 1);
}

TEST(DatabaseLifetime, QueryResultKeepsPoolAliveAfterDatabaseDestroyed) {
    MockCliDriver driver;
    driver.resultSets.push_back(idName(1, "a"));
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    std::optional<Database> db = Database::open(driver, "X", cfg).value();
    ASSERT_EQ(driver.connectCalls, 1);

    {
        auto qr = db->query("SELECT id, name FROM t");
        ASSERT_TRUE(qr.ok());

        // Destroy the only Database handle while the QueryResult is still alive.
        db.reset();

        // The QueryResult must keep the pool (and its leased connection) alive:
        // nothing is disconnected yet, and the borrowed cursor is still usable
        // — i.e. no use-after-free of pool/connection/driver state.
        EXPECT_EQ(driver.disconnectCalls, 0);
        int rows = 0;
        for (auto& row : qr.value()) {
            (void)row;
            ++rows;
        }
        EXPECT_EQ(rows, 1);
    }  // QueryResult destroyed here → connection finally torn down

    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(DatabaseLifetime, SharedDriverOutlivesDatabaseViaQueryResult) {
    auto driver = std::make_shared<MockCliDriver>();
    driver->resultSets.push_back(idName(1, "a"));
    std::weak_ptr<MockCliDriver> weak = driver;

    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    std::optional<Database> db = Database::open(driver, "X", cfg).value();
    driver.reset();                // caller drops its handle → the Database now co-owns it
    ASSERT_FALSE(weak.expired());  // Database keeps the shared driver alive

    {
        auto qr = db->query("SELECT id, name FROM t");
        ASSERT_TRUE(qr.ok());
        db.reset();  // destroy the only Database handle

        // The QueryResult keeps BOTH the pool and the (shared) driver alive, so
        // the borrowed cursor is still safe to use — no dangling external driver.
        EXPECT_FALSE(weak.expired());
        int rows = 0;
        for (auto& row : qr.value()) {
            (void)row;
            ++rows;
        }
        EXPECT_EQ(rows, 1);
    }  // QueryResult destroyed → last driver reference released

    EXPECT_TRUE(weak.expired());
}

TEST(DatabaseLifetime, ScopedTransactionKeepsPoolAliveAfterDatabaseDestroyed) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    std::optional<Database> db = Database::open(driver, "X", cfg).value();
    ASSERT_EQ(driver.connectCalls, 1);

    {
        auto tx = db->begin();
        ASSERT_TRUE(tx.ok());

        // Destroy the only Database handle mid-transaction.
        db.reset();

        // The ScopedTransaction keeps the pool/connection alive: not yet
        // disconnected, and still usable for execute/commit.
        EXPECT_EQ(driver.disconnectCalls, 0);
        ASSERT_TRUE(tx.value().execute("UPDATE t SET a=?", 1).ok());
        ASSERT_TRUE(tx.value().commit().ok());
    }  // ScopedTransaction destroyed here → connection finally torn down

    EXPECT_EQ(driver.commitCalls, 1);
    EXPECT_EQ(driver.disconnectCalls, 1);
}

namespace {
halcyon::Error connError(const char* msg = "dead") {
    halcyon::Error e;
    e.code = halcyon::ErrorCode::Connection;
    e.message = msg;
    return e;
}
}  // namespace

TEST(DatabaseBrokenConn, BeginFailureDiscardsConnection) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    auto db = Database::open(driver, "X", cfg).value();
    ASSERT_EQ(driver.connectCalls, 1);

    driver.txnErrors.push_back(connError());  // begin's setAutoCommit(false) fails
    auto tx = db.begin();
    ASSERT_FALSE(tx.ok());
    // begin() can leave autocommit in an unknown state → discard the connection.
    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(DatabaseBrokenConn, CommitFailureDiscardsConnection) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    auto db = Database::open(driver, "X", cfg).value();
    {
        auto tx = db.begin();
        ASSERT_TRUE(tx.ok());
        driver.txnErrors.push_back(connError());  // commit fails
        EXPECT_FALSE(tx.value().commit().ok());
    }  // ScopedTransaction destroyed
    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(DatabaseBrokenConn, ImplicitRollbackFailureDiscardsConnection) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    auto db = Database::open(driver, "X", cfg).value();
    {
        auto tx = db.begin();
        ASSERT_TRUE(tx.ok());
        // No commit/rollback: the destructor's implicit rollback fails.
        driver.txnErrors.push_back(connError());
    }  // ScopedTransaction destroyed → implicit rollback fails → discard
    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(DatabaseBrokenConn, SuccessfulTransactionKeepsConnection) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    auto db = Database::open(driver, "X", cfg).value();
    {
        auto tx = db.begin();
        ASSERT_TRUE(tx.ok());
        ASSERT_TRUE(tx.value().execute("UPDATE t SET a=?", 1).ok());
        ASSERT_TRUE(tx.value().commit().ok());
    }  // clean commit → connection returns to the pool, not discarded
    EXPECT_EQ(driver.disconnectCalls, 0);

    auto tx2 = db.begin();  // reuses the same connection (no new connect)
    ASSERT_TRUE(tx2.ok());
    EXPECT_EQ(driver.connectCalls, 1);
}

TEST(DatabaseBrokenConn, MidStreamFetchBlockConnectionErrorDiscardsConnection) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    driver.resultSets.push_back(idName(1, "a"));
    driver.fetchBlockError = connError();
    driver.failFetchBlockOnCall = 1;  // first fetchBlock call fails
    auto db = Database::open(driver, "X", cfg).value();
    {
        auto qr = db.query("SELECT id, name FROM t");
        ASSERT_TRUE(qr.ok());
        int rows = 0;
        for (auto& row : qr.value()) {
            (void)row;
            ++rows;
        }
        EXPECT_EQ(rows, 0);
        // The fetchBlock failure must be recorded so the dead connection is discarded.
        ASSERT_FALSE(qr.value().ok());
        ASSERT_TRUE(qr.value().error().has_value());
        EXPECT_EQ(qr.value().error()->code, halcyon::ErrorCode::Connection);
    }  // QueryResult destroyed → dead connection discarded
    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(DatabaseBrokenConn, MidStreamFetchConnectionErrorDiscardsConnection) {
    MockCliDriver driver;
    PoolConfig cfg = noThread();
    cfg.min = 1;
    cfg.max = 1;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id", "name"},
        {{halcyon::detail::cli::Value{std::int64_t{1}},
          halcyon::detail::cli::Value{std::string{"a"}}},
         {halcyon::detail::cli::Value{std::int64_t{2}},
          halcyon::detail::cli::Value{std::string{"b"}}}}});
    driver.fetchBlockSize = 1;  // one row per block
    driver.fetchBlockError = connError();
    driver.failFetchBlockOnCall = 2;  // second fetchBlock (after first row) fails
    auto db = Database::open(driver, "X", cfg).value();
    {
        auto qr = db.query("SELECT id, name FROM t");
        ASSERT_TRUE(qr.ok());
        int rows = 0;
        for (auto& row : qr.value()) {
            (void)row;
            ++rows;
        }
        EXPECT_EQ(rows, 1);
        ASSERT_FALSE(qr.value().ok());
        ASSERT_TRUE(qr.value().error().has_value());
        EXPECT_EQ(qr.value().error()->code, halcyon::ErrorCode::Connection);
    }  // QueryResult destroyed → dead connection discarded
    EXPECT_EQ(driver.disconnectCalls, 1);
}

TEST(FunctionalApi, FreeFunctionsDelegateToDatabase) {
    MockCliDriver driver;
    auto db = Database::open(driver, "X", noThread()).value();

    // execute() drains execRowCounts (mock attaches resultSets first, so none
    // must be queued yet for the count to be returned).
    driver.execRowCounts.push_back(2);
    auto n = halcyon::execute(db, "UPDATE t SET a=? WHERE id=?", 1, 3);
    ASSERT_TRUE(n.ok());
    EXPECT_EQ(n.value(), 2);

    driver.resultSets.push_back(idName(3, "c"));
    auto people = halcyon::query_as<Person>(db, "SELECT id, name FROM t");
    ASSERT_TRUE(people.ok());
    ASSERT_EQ(people.value().size(), 1u);
    EXPECT_EQ(people.value()[0].id, 3);
}
