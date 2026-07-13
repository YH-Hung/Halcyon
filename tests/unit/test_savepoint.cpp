#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include "capturing_logger.hpp"
#include "halcyon/halcyon.hpp"
#include "mock_cli_driver.hpp"

using halcyon::Connection;
using halcyon::ErrorCode;
using halcyon::testing::MockCliDriver;

namespace {

bool prepared(const MockCliDriver& drv, const std::string& sql) {
    return std::find(drv.preparedSql.begin(), drv.preparedSql.end(), sql) !=
           drv.preparedSql.end();
}

struct Fixture {
    MockCliDriver drv;
    halcyon::testing::CapturingLogger logger;
    std::unique_ptr<Connection> conn;
    Fixture() {
        auto c = Connection::open(drv, {"dsn"}, 0, nullptr, &logger);
        conn = std::make_unique<Connection>(std::move(c.value()));
    }
};

}  // namespace

TEST(Savepoint, AutoNamesCountPerTransaction) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto sp1 = tx.value().savepoint();
    ASSERT_TRUE(sp1.ok());
    auto sp2 = tx.value().savepoint();
    ASSERT_TRUE(sp2.ok());
    EXPECT_EQ(sp1.value().name(), "halcyon_sp_1");
    EXPECT_EQ(sp2.value().name(), "halcyon_sp_2");
    EXPECT_TRUE(prepared(f.drv,
                         "SAVEPOINT halcyon_sp_1 ON ROLLBACK RETAIN CURSORS"));
    EXPECT_TRUE(prepared(f.drv,
                         "SAVEPOINT halcyon_sp_2 ON ROLLBACK RETAIN CURSORS"));
    ASSERT_TRUE(sp2.value().release().ok());
    ASSERT_TRUE(sp1.value().release().ok());
}

TEST(Savepoint, ExplicitNameValidation) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    const std::size_t before = f.drv.preparedSql.size();
    for (const char* bad :
         {"", "1abc", "a b", "a;DROP TABLE t--", "SYSfoo", "sysx", "a-b"}) {
        auto sp = tx.value().savepoint(bad);
        ASSERT_FALSE(sp.ok()) << bad;
        EXPECT_EQ(sp.error().code, ErrorCode::InvalidArgument) << bad;
    }
    auto tooLong = tx.value().savepoint(std::string(129, 'a'));
    ASSERT_FALSE(tooLong.ok());
    EXPECT_EQ(f.drv.preparedSql.size(), before);  // nothing reached the driver

    auto good = tx.value().savepoint("stage_1");
    ASSERT_TRUE(good.ok());
    EXPECT_TRUE(prepared(f.drv, "SAVEPOINT stage_1 ON ROLLBACK RETAIN CURSORS"));
    ASSERT_TRUE(good.value().release().ok());
}

TEST(Savepoint, RollbackAndReleaseAreOneShot) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto sp = tx.value().savepoint();
    ASSERT_TRUE(sp.ok());
    ASSERT_TRUE(sp.value().rollback().ok());
    EXPECT_TRUE(prepared(f.drv, "ROLLBACK TO SAVEPOINT halcyon_sp_1"));
    const std::size_t after = f.drv.preparedSql.size();
    EXPECT_TRUE(sp.value().rollback().ok());  // inert no-op
    EXPECT_TRUE(sp.value().release().ok());   // inert no-op
    EXPECT_EQ(f.drv.preparedSql.size(), after);
    EXPECT_FALSE(sp.value().active());
}

TEST(Savepoint, DestructorRollsBackAndReleases) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    { auto sp = tx.value().savepoint(); ASSERT_TRUE(sp.ok()); }
    EXPECT_TRUE(prepared(f.drv, "ROLLBACK TO SAVEPOINT halcyon_sp_1"));
    EXPECT_TRUE(prepared(f.drv, "RELEASE SAVEPOINT halcyon_sp_1"));
}

TEST(Savepoint, DestructorAfterReleaseDoesNothing) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    {
        auto sp = tx.value().savepoint();
        ASSERT_TRUE(sp.ok());
        ASSERT_TRUE(sp.value().release().ok());
    }
    EXPECT_FALSE(prepared(f.drv, "ROLLBACK TO SAVEPOINT halcyon_sp_1"));
}

TEST(Savepoint, FailedRollbackPoisonsTransactionAndLogs) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto sp = tx.value().savepoint();
    ASSERT_TRUE(sp.ok());
    halcyon::Error dead;
    dead.code = ErrorCode::Connection;
    f.drv.executeErrors.push_back(dead);  // the ROLLBACK TO ... execute fails
    auto r = sp.value().rollback();
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(sp.value().poisoned());
    EXPECT_TRUE(tx.value().poisoned());
    EXPECT_EQ(f.logger.count("savepoint.poisoned"), 1u);
}

TEST(Savepoint, InactiveTransactionIsRejected) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    ASSERT_TRUE(tx.value().commit().ok());
    auto sp = tx.value().savepoint();
    ASSERT_FALSE(sp.ok());
    EXPECT_EQ(sp.error().code, ErrorCode::InvalidState);
}

TEST(NestedTransaction, SuccessReleases) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto r = tx.value().nested(
        [](halcyon::Transaction& inner) -> halcyon::Result<std::int64_t> {
            return inner.execute("INSERT INTO t VALUES (1)");
        });
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(prepared(f.drv, "SAVEPOINT halcyon_sp_1 ON ROLLBACK RETAIN CURSORS"));
    EXPECT_TRUE(prepared(f.drv, "RELEASE SAVEPOINT halcyon_sp_1"));
    EXPECT_FALSE(prepared(f.drv, "ROLLBACK TO SAVEPOINT halcyon_sp_1"));
}

TEST(NestedTransaction, ErrorResultRollsBackToSavepoint) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto r = tx.value().nested(
        [](halcyon::Transaction&) -> halcyon::Result<std::int64_t> {
            halcyon::Error e;
            e.code = halcyon::ErrorCode::Constraint;
            e.message = "dup";
            return e;
        });
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, halcyon::ErrorCode::Constraint);
    EXPECT_TRUE(prepared(f.drv, "ROLLBACK TO SAVEPOINT halcyon_sp_1"));
    EXPECT_TRUE(tx.value().active());  // outer transaction still usable
}

TEST(NestedTransaction, ExceptionRollsBackAndRethrows) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    EXPECT_THROW(
        tx.value().nested(
            [](halcyon::Transaction&) -> halcyon::Result<std::int64_t> {
                throw std::runtime_error("boom");
            }),
        std::runtime_error);
    EXPECT_TRUE(prepared(f.drv, "ROLLBACK TO SAVEPOINT halcyon_sp_1"));
}

TEST(NestedTransaction, CommitInsideScopeIsInvalidState) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto r = tx.value().nested(
        [](halcyon::Transaction& inner) -> halcyon::Result<std::int64_t> {
            auto c = inner.commit();  // misuse
            (void)c;
            return std::int64_t{0};
        });
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.error().code, halcyon::ErrorCode::InvalidState);
}

TEST(NestedTransaction, NestedInNestedUsesSecondSavepoint) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto r = tx.value().nested(
        [](halcyon::Transaction& t1) -> halcyon::Result<std::int64_t> {
            return t1.nested(
                [](halcyon::Transaction& t2) -> halcyon::Result<std::int64_t> {
                    return t2.execute("INSERT INTO t VALUES (2)");
                });
        });
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(prepared(f.drv, "RELEASE SAVEPOINT halcyon_sp_2"));
    EXPECT_TRUE(prepared(f.drv, "RELEASE SAVEPOINT halcyon_sp_1"));
}
