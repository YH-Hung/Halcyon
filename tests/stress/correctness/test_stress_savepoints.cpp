#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "concurrent_fake_driver.hpp"
#include "halcyon/halcyon.hpp"

using halcyon::Database;
using halcyon::stress::ConcurrentFakeDriver;

// Hammers transactions + savepoints + isolation from many threads. Run under
// TSan/ASan to prove race/deadlock freedom and that pool session state is always
// restored (or the connection discarded) so no lease leaks.
TEST(StressSavepoints, ConcurrentTransactionsWithSavepointsAndIsolation) {
    auto fake = std::make_shared<ConcurrentFakeDriver>();
    halcyon::PoolConfig cfg;
    cfg.min = 4;
    cfg.max = 8;
    cfg.startMaintenanceThread = true;
    auto db = Database::open(fake, "dsn", cfg);
    ASSERT_TRUE(db.ok()) << db.error().message;

    constexpr int kThreads = 8;
    constexpr int kIters = 200;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kIters; ++i) {
                const auto iso = (i % 2 == 0)
                                     ? halcyon::Isolation::CursorStability
                                     : halcyon::Isolation::RepeatableRead;
                auto r = db.value().transaction(
                    iso,
                    [&](halcyon::Transaction& tx)
                        -> halcyon::Result<std::int64_t> {
                        auto a = tx.execute("INSERT INTO t VALUES (?)", t);
                        if (!a.ok()) return a;
                        auto sp = tx.savepoint();
                        if (!sp.ok()) return sp.error();
                        auto b = tx.execute("INSERT INTO t VALUES (?)", i);
                        if (!b.ok()) return b;
                        if (i % 3 == 0) {
                            auto rb = sp.value().rollback();
                            if (!rb.ok()) return rb.error();
                        } else {
                            auto rel = sp.value().release();
                            if (!rel.ok()) return rel.error();
                        }
                        return tx.nested(
                            [&](halcyon::Transaction& inner)
                                -> halcyon::Result<std::int64_t> {
                                return inner.execute("INSERT INTO t VALUES (?)",
                                                     i + t);
                            });
                    });
                if (!r.ok()) ++failures;
            }
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(failures.load(), 0);
    // Every lease returned: nothing left active.
    EXPECT_EQ(db.value().pool().active_count(), 0u);
}
