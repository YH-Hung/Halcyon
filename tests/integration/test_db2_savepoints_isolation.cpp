#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "halcyon/halcyon.hpp"

using halcyon::Connection;
using halcyon::Isolation;

namespace {
std::optional<std::string> dsn() {
    if (const char* v = std::getenv("HALCYON_TEST_DSN")) return std::string(v);
    return std::nullopt;
}
}  // namespace

// queryAs<T> requires a reflected struct (tuples are for Row::as<>).
struct CountRow {
    std::int64_t c;
};
HALCYON_REFLECT(CountRow, c);

class Db2TxnV11 : public ::testing::Test {
protected:
    void SetUp() override {
        auto d = dsn();
        if (!d) GTEST_SKIP() << "HALCYON_TEST_DSN not set; skipping live Db2 test";
        driver_ = halcyon::detail::cli::make_db2_cli_driver();
        auto c = Connection::open(*driver_, {*d});
        ASSERT_TRUE(c.ok()) << c.error().message;
        conn_ = std::make_unique<Connection>(std::move(c.value()));
        conn_->execute("DROP TABLE halcyon_v11_txn");  // ignore error if absent
        ASSERT_TRUE(conn_->execute("CREATE TABLE halcyon_v11_txn(id INT NOT NULL)")
                        .ok());
    }

    std::int64_t count() {
        auto rows =
            conn_->queryAs<CountRow>("SELECT COUNT(*) FROM halcyon_v11_txn");
        return rows.value().at(0).c;
    }

    std::unique_ptr<halcyon::detail::cli::ICliDriver> driver_;
    std::unique_ptr<Connection> conn_;
};

// Spike: sequential savepoints, rollback-to visibility, release.
TEST_F(Db2TxnV11, SavepointRollbackUndoesOnlyLaterWork) {
    auto tx = conn_->begin();
    ASSERT_TRUE(tx.ok()) << tx.error().message;
    ASSERT_TRUE(tx.value().execute("INSERT INTO halcyon_v11_txn VALUES (1)").ok());
    auto sp = tx.value().savepoint("stage1");
    ASSERT_TRUE(sp.ok()) << sp.error().message;
    ASSERT_TRUE(tx.value().execute("INSERT INTO halcyon_v11_txn VALUES (2)").ok());
    EXPECT_EQ(count(), 2);
    ASSERT_TRUE(sp.value().rollback().ok());
    EXPECT_EQ(count(), 1);  // row 2 undone, row 1 kept
    ASSERT_TRUE(tx.value().commit().ok());
    EXPECT_EQ(count(), 1);
}

// Spike: Db2 nested savepoints; ROLLBACK TO an outer one destroys inner ones.
TEST_F(Db2TxnV11, NestedSavepointsAndOuterRollback) {
    auto tx = conn_->begin();
    ASSERT_TRUE(tx.ok());
    auto sp1 = tx.value().savepoint("outer_sp");
    ASSERT_TRUE(sp1.ok()) << sp1.error().message;
    ASSERT_TRUE(tx.value().execute("INSERT INTO halcyon_v11_txn VALUES (1)").ok());
    {
        auto sp2 = tx.value().savepoint("inner_sp");
        ASSERT_TRUE(sp2.ok()) << sp2.error().message;  // nested savepoints OK (9.7+)
        ASSERT_TRUE(tx.value().execute("INSERT INTO halcyon_v11_txn VALUES (2)").ok());
        ASSERT_TRUE(sp2.value().release().ok());
    }
    ASSERT_TRUE(sp1.value().rollback().ok());
    EXPECT_EQ(count(), 0);
    ASSERT_TRUE(tx.value().commit().ok());
}

// Spike (raw SQL, bypassing the client-side guard): demonstrates the actual Db2
// name-reuse semantics that motivate the policy — same-name reuse REPLACES the
// server-side savepoint, and unquoted identifiers fold case (spec §8).
TEST_F(Db2TxnV11, RawSavepointReuseReplacesAndFoldsCase) {
    auto tx = conn_->begin();
    ASSERT_TRUE(tx.ok());
    // Replacement: reissuing SAVEPOINT sp moves its boundary forward.
    ASSERT_TRUE(tx.value().execute("SAVEPOINT sp ON ROLLBACK RETAIN CURSORS").ok());
    ASSERT_TRUE(tx.value().execute("INSERT INTO halcyon_v11_txn VALUES (1)").ok());
    ASSERT_TRUE(  // Db2 accepts the reuse and replaces the earlier savepoint
        tx.value().execute("SAVEPOINT sp ON ROLLBACK RETAIN CURSORS").ok());
    ASSERT_TRUE(tx.value().execute("INSERT INTO halcyon_v11_txn VALUES (2)").ok());
    ASSERT_TRUE(tx.value().execute("ROLLBACK TO SAVEPOINT sp").ok());
    EXPECT_EQ(count(), 1);  // rolled back to the SECOND savepoint, not the first
    ASSERT_TRUE(tx.value().execute("RELEASE SAVEPOINT sp").ok());

    // Case folding: an unquoted name folds to uppercase, so RELEASE with a
    // different casing addresses the very same savepoint.
    ASSERT_TRUE(tx.value()
                    .execute("SAVEPOINT case_alias ON ROLLBACK RETAIN CURSORS")
                    .ok());
    EXPECT_TRUE(tx.value().execute("RELEASE SAVEPOINT CASE_ALIAS").ok());

    ASSERT_TRUE(tx.value().rollback().ok());
}

// Spike: same-name savepoint reuse. Db2 replaces the server-side savepoint on
// same-name reuse (silently retargeting a still-active guard), so Halcyon
// rejects reusing the name of a live savepoint client-side (spec §5 C.2 / §8).
TEST_F(Db2TxnV11, DuplicateActiveSavepointNameIsRejected) {
    auto tx = conn_->begin();
    ASSERT_TRUE(tx.ok());
    auto sp1 = tx.value().savepoint("reused");
    ASSERT_TRUE(sp1.ok()) << sp1.error().message;
    ASSERT_TRUE(tx.value().execute("INSERT INTO halcyon_v11_txn VALUES (1)").ok());
    // Reuse while sp1 is live is rejected before any SQL reaches Db2.
    auto sp2 = tx.value().savepoint("reused");
    ASSERT_FALSE(sp2.ok());
    EXPECT_EQ(sp2.error().code, halcyon::ErrorCode::InvalidArgument);
    // sp1 still targets its original boundary and rolls back the insert.
    ASSERT_TRUE(sp1.value().rollback().ok());
    EXPECT_EQ(count(), 0);
    ASSERT_TRUE(tx.value().commit().ok());
}

// Spike: ROLLBACK TO an outer savepoint destroys savepoints established after
// it. Unlike NestedSavepointsAndOuterRollback, the inner savepoint is left LIVE
// (not released) so the "destroys later savepoints" rule is actually exercised.
TEST_F(Db2TxnV11, OuterRollbackDestroysLaterSavepoint) {
    auto tx = conn_->begin();
    ASSERT_TRUE(tx.ok());
    auto sp1 = tx.value().savepoint("outer_sp");
    ASSERT_TRUE(sp1.ok()) << sp1.error().message;
    ASSERT_TRUE(tx.value().execute("INSERT INTO halcyon_v11_txn VALUES (1)").ok());
    auto sp2 = tx.value().savepoint("inner_sp");
    ASSERT_TRUE(sp2.ok()) << sp2.error().message;
    ASSERT_TRUE(tx.value().execute("INSERT INTO halcyon_v11_txn VALUES (2)").ok());
    EXPECT_EQ(count(), 2);

    // Roll back to the OUTER savepoint: undoes both inserts and destroys the
    // later (inner) savepoint server-side.
    ASSERT_TRUE(sp1.value().rollback().ok());
    EXPECT_EQ(count(), 0);

    // The inner savepoint no longer exists: rolling back to it now fails.
    auto innerRb = sp2.value().rollback();
    EXPECT_FALSE(innerRb.ok());

    // outer_sp itself survives a ROLLBACK TO, so the transaction is still usable.
    ASSERT_TRUE(tx.value().rollback().ok());
}

TEST_F(Db2TxnV11, NestedScopeErrorKeepsOuterWork) {
    auto tx = conn_->begin();
    ASSERT_TRUE(tx.ok());
    ASSERT_TRUE(tx.value().execute("INSERT INTO halcyon_v11_txn VALUES (1)").ok());
    auto r = tx.value().nested(
        [](halcyon::Transaction& inner) -> halcyon::Result<std::int64_t> {
            auto i = inner.execute("INSERT INTO halcyon_v11_txn VALUES (2)");
            if (!i.ok()) return i;
            halcyon::Error e;
            e.code = halcyon::ErrorCode::Constraint;
            e.message = "business rule failed";
            return e;
        });
    EXPECT_FALSE(r.ok());
    ASSERT_TRUE(tx.value().commit().ok());
    EXPECT_EQ(count(), 1);  // outer insert persisted, nested insert undone
}

// UR sees another connection's uncommitted insert; CS (with a lock timeout)
// errors instead of reading it.
TEST_F(Db2TxnV11, UncommittedReadSeesInFlightRowAndCursorStabilityDoesNot) {
    auto d = dsn();
    auto c2r = Connection::open(*driver_, {*d});
    ASSERT_TRUE(c2r.ok());
    Connection writer = std::move(c2r.value());
    auto wtx = writer.begin();
    ASSERT_TRUE(wtx.ok());
    ASSERT_TRUE(wtx.value().execute("INSERT INTO halcyon_v11_txn VALUES (42)").ok());
    // Uncommitted at this point.

    {
        auto ur = conn_->begin(Isolation::UncommittedRead);
        ASSERT_TRUE(ur.ok()) << ur.error().message;
        auto rows = ur.value().queryAs<CountRow>(
            "SELECT COUNT(*) FROM halcyon_v11_txn");
        ASSERT_TRUE(rows.ok()) << rows.error().message;
        EXPECT_EQ(rows.value().at(0).c, 1);  // dirty read visible
        ASSERT_TRUE(ur.value().commit().ok());
    }
    {
        // SPIKE FINDING: with Db2's default Currently Committed semantics
        // (cur_commit=ON, the default since Db2 9.7), a Cursor Stability reader
        // does NOT block on the writer's uncommitted-row lock and does NOT see
        // the in-flight row — it reads the last committed version instead. So
        // the read succeeds immediately and the uncommitted insert is invisible
        // (count 0), rather than blocking into a lock timeout as originally
        // assumed. (Recorded in the v1.1 design spec §9.)
        auto cs = conn_->begin(Isolation::CursorStability);
        ASSERT_TRUE(cs.ok());
        auto rows = cs.value().queryAs<CountRow>(
            "SELECT COUNT(*) FROM halcyon_v11_txn");
        ASSERT_TRUE(rows.ok()) << rows.error().message;  // no block, no timeout
        EXPECT_EQ(rows.value().at(0).c, 0);  // uncommitted insert not visible
        ASSERT_TRUE(cs.value().commit().ok());
    }
    ASSERT_TRUE(wtx.value().rollback().ok());
}

TEST_F(Db2TxnV11, IsolationOverrideRoundTripSmoke) {
    for (auto iso : {Isolation::UncommittedRead, Isolation::CursorStability,
                     Isolation::ReadStability, Isolation::RepeatableRead}) {
        auto tx = conn_->begin(iso);
        ASSERT_TRUE(tx.ok()) << tx.error().message;
        ASSERT_TRUE(
            tx.value().execute("INSERT INTO halcyon_v11_txn VALUES (7)").ok());
        ASSERT_TRUE(tx.value().rollback().ok());
    }
}
