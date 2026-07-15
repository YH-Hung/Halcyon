#include <gtest/gtest.h>

#include <algorithm>
#include <clocale>
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
    {
        auto sp = tx.value().savepoint();
        ASSERT_TRUE(sp.ok());
    }
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
    // The savepoint guard must be disarmed: no ROLLBACK TO / RELEASE may be
    // issued against the now-ended transaction (against live Db2 those would
    // fail and poison the connection). The mock succeeds silently, so assert on
    // the emitted SQL rather than on poisoning.
    EXPECT_FALSE(prepared(f.drv, "ROLLBACK TO SAVEPOINT halcyon_sp_1"));
    EXPECT_FALSE(prepared(f.drv, "RELEASE SAVEPOINT halcyon_sp_1"));
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

TEST(Savepoint, DuplicateActiveNameIsRejected) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto sp1 = tx.value().savepoint("dup");
    ASSERT_TRUE(sp1.ok());
    // A second guard with the same still-active name is rejected: Db2 would
    // replace the server-side savepoint and silently retarget sp1.
    auto sp2 = tx.value().savepoint("dup");
    ASSERT_FALSE(sp2.ok());
    EXPECT_EQ(sp2.error().code, halcyon::ErrorCode::InvalidArgument);
    // Releasing sp1 frees the name for reuse.
    ASSERT_TRUE(sp1.value().release().ok());
    auto sp3 = tx.value().savepoint("dup");
    EXPECT_TRUE(sp3.ok());
    ASSERT_TRUE(sp3.value().release().ok());
}

TEST(Savepoint, NameFreedAfterGuardDestroyed) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    {
        auto sp = tx.value().savepoint("scoped");
        ASSERT_TRUE(sp.ok());
    }  // dtor frees
    auto sp2 = tx.value().savepoint("scoped");
    EXPECT_TRUE(sp2.ok());
    ASSERT_TRUE(sp2.value().release().ok());
}

TEST(NestedTransaction, CommitThenThrowInsideScopeDisarmsSavepoint) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    EXPECT_THROW(
        tx.value().nested(
            [](halcyon::Transaction& inner) -> halcyon::Result<std::int64_t> {
                auto c = inner.commit();  // ends the transaction
                (void)c;
                throw std::runtime_error("after commit");
            }),
        std::runtime_error);
    // The savepoint guard must be disarmed on the throw-after-end path too: no
    // ROLLBACK TO / RELEASE against the ended transaction.
    EXPECT_FALSE(prepared(f.drv, "ROLLBACK TO SAVEPOINT halcyon_sp_1"));
    EXPECT_FALSE(prepared(f.drv, "RELEASE SAVEPOINT halcyon_sp_1"));
}

TEST(Savepoint, DuplicateNameIsCaseInsensitive) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto sp1 = tx.value().savepoint("case_alias");
    ASSERT_TRUE(sp1.ok());
    // Db2 folds unquoted identifiers, so a different casing is the SAME
    // server-side savepoint and must be rejected.
    auto sp2 = tx.value().savepoint("CASE_ALIAS");
    ASSERT_FALSE(sp2.ok());
    EXPECT_EQ(sp2.error().code, halcyon::ErrorCode::InvalidArgument);
    // The name frees case-insensitively once its guard is released.
    ASSERT_TRUE(sp1.value().release().ok());
    auto sp3 = tx.value().savepoint("Case_Alias");
    EXPECT_TRUE(sp3.ok());
    ASSERT_TRUE(sp3.value().release().ok());
}

TEST(Savepoint, ExplicitNameCollidesWithAutoNameCaseInsensitively) {
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto autoSp = tx.value().savepoint();  // halcyon_sp_1
    ASSERT_TRUE(autoSp.ok());
    EXPECT_EQ(autoSp.value().name(), "halcyon_sp_1");   // original spelling kept
    auto clash = tx.value().savepoint("HALCYON_SP_1");  // same, case-folded
    ASSERT_FALSE(clash.ok());
    EXPECT_EQ(clash.error().code, halcyon::ErrorCode::InvalidArgument);
    ASSERT_TRUE(autoSp.value().release().ok());
}

namespace {
// Restores LC_CTYPE on scope exit so a locale flip can't leak into other tests.
struct LocaleGuard {
    std::string saved;
    LocaleGuard() {
        const char* s = std::setlocale(LC_CTYPE, nullptr);
        saved = s ? s : "C";
    }
    ~LocaleGuard() { std::setlocale(LC_CTYPE, saved.c_str()); }
};
}  // namespace

TEST(Savepoint, NameGateIsAsciiOnlyRegardlessOfLocale) {
    LocaleGuard guard;
    // Best-effort switch to a Latin-1 locale where <cctype> would classify 0xE9
    // (e-acute) as a letter and fold 0xE9/0xC9 together. A no-op if the locale
    // is not installed; the assertions are ASCII-only either way.
    std::setlocale(LC_CTYPE, "fr_FR.ISO8859-1");

    using halcyon::detail::valid_savepoint_name;
    EXPECT_FALSE(valid_savepoint_name("\xE9"));     // non-ASCII first char
    EXPECT_FALSE(valid_savepoint_name("caf\xE9"));  // non-ASCII trailing char
    EXPECT_TRUE(valid_savepoint_name("cafe"));      // ASCII still accepted

    // The reuse comparator must not conflate e-acute (0xE9) and E-acute (0xC9):
    // ASCII folding leaves non-ASCII bytes unchanged, so they stay distinct.
    halcyon::detail::SavepointNameLess less;
    const std::string lo("\xE9"), up("\xC9");
    EXPECT_TRUE(less(lo, up) != less(up, lo));  // strictly ordered => not equal

    // And through the public API: a non-ASCII savepoint name is rejected.
    Fixture f;
    auto tx = f.conn->begin();
    ASSERT_TRUE(tx.ok());
    auto sp = tx.value().savepoint("caf\xE9");
    ASSERT_FALSE(sp.ok());
    EXPECT_EQ(sp.error().code, halcyon::ErrorCode::InvalidArgument);
}
