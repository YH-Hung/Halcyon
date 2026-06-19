#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <vector>

#include "halcyon/async.hpp"
#include "halcyon/pool.hpp"
#include "halcyon/result.hpp"
#include "mock_cli_driver.hpp"

using halcyon::ConnectionPool;
using halcyon::Executor;
using halcyon::PoolConfig;
using halcyon::Result;
using halcyon::testing::MockCliDriver;

TEST(Executor, SubmitReturnsFutureWithValue) {
    Executor ex(2);
    auto f = ex.submit([] { return 6 * 7; });
    EXPECT_EQ(f.get(), 42);
}

TEST(Executor, RunsManyTasks) {
    Executor ex(4);
    std::vector<std::future<int>> fs;
    for (int i = 0; i < 16; ++i) fs.push_back(ex.submit([i] { return i; }));
    int sum = 0;
    for (auto& f : fs) sum += f.get();
    EXPECT_EQ(sum, 120);
}

TEST(Executor, DrainsOutstandingTasksOnDestruction) {
    std::atomic<int> done{0};
    {
        Executor ex(2);
        for (int i = 0; i < 8; ++i)
            ex.submit([&done] { done.fetch_add(1); });
    }  // dtor joins after draining the queue
    EXPECT_EQ(done.load(), 8);
}

TEST(AsyncWithConnection, RunsQueryOnPooledConnection) {
    MockCliDriver driver;
    driver.resultSets.push_back(MockCliDriver::ScriptedRows{
        {"id"}, {{halcyon::detail::cli::Value{std::int64_t{5}}},
                 {halcyon::detail::cli::Value{std::int64_t{9}}}}});
    PoolConfig cfg;
    cfg.startMaintenanceThread = false;
    auto pool = ConnectionPool::create(driver, {"x"}, cfg).value();
    Executor ex(2);

    auto f = halcyon::async_with_connection(
        ex, *pool, [](halcyon::Connection& c) -> Result<std::int64_t> {
            auto rs = c.query("SELECT id FROM t");
            if (!rs.ok()) return rs.error();
            std::int64_t sum = 0;
            for (auto& row : rs.value()) sum += std::get<0>(row.as<std::int64_t>());
            return sum;
        });
    auto r = f.get();
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.value(), 14);
}
