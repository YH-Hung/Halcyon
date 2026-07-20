// C++20 coroutine style, mirroring the orders sample's flow. Requires a live
// DSN in HALCYON_TEST_DSN to actually run; otherwise it prints usage and
// exits 0 so the build stays self-contained.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "halcyon/coro.hpp"
#include "halcyon/halcyon.hpp"

struct OrderRow {
    std::int64_t id;
    std::string status;
};
HALCYON_REFLECT(OrderRow, id, status);

namespace {

halcyon::coro::Task<halcyon::Result<int>> orders_flow(halcyon::Database db) {
    using halcyon::Result;

    // 1. Reset + seed inside one awaitable transaction (commit on ok).
    auto seeded = co_await halcyon::coro::transaction(
        db, [](halcyon::Transaction& tx) -> Result<int> {
            tx.execute("DROP TABLE halcyon_coro_orders");  // ignore error
            auto c = tx.execute(
                "CREATE TABLE halcyon_coro_orders("
                "id BIGINT NOT NULL, status VARCHAR(16))");
            if (!c.ok()) return c.error();
            auto i = tx.executeBatch(
                "INSERT INTO halcyon_coro_orders VALUES (?, ?)",
                halcyon::batchOf({std::make_tuple(1, std::string{"NEW"}),
                                  std::make_tuple(2, std::string{"NEW"}),
                                  std::make_tuple(3, std::string{"SHIPPED"})}));
            if (!i.ok()) return i.error();
            return Result<int>(static_cast<int>(i.value()));
        });
    if (!seeded.ok()) co_return Result<int>(seeded.error());
    std::cout << "seeded " << seeded.value() << " orders\n";

    // 2. Awaitable typed query.
    auto open = co_await halcyon::coro::query<OrderRow>(
        db, "SELECT id, status FROM halcyon_coro_orders WHERE status = ?",
        std::string{"NEW"});
    if (!open.ok()) co_return Result<int>(open.error());
    for (const auto& o : open.value())
        std::cout << "open order " << o.id << " (" << o.status << ")\n";

    // 3. Awaitable streaming read (row at a time).
    auto sqr = co_await halcyon::coro::queryStreaming(
        db, "SELECT id, status FROM halcyon_coro_orders ORDER BY id");
    if (!sqr.ok()) co_return Result<int>(sqr.error());
    auto sq = std::move(sqr.value());
    int rows = 0;
    while (auto row = co_await sq.next()) {
        auto id = row->get<std::int64_t>(0);
        auto status = row->get<std::string>(1);
        if (!id.ok() || !status.ok()) break;
        std::cout << "streamed " << id.value() << " -> " << status.value()
                  << "\n";
        ++rows;
    }
    if (!sq.ok()) co_return Result<int>(*sq.error());
    co_return Result<int>(rows);
}

}  // namespace

int main() {
    const char* dsn = std::getenv("HALCYON_TEST_DSN");
    if (!dsn) {
        std::cout << "Set HALCYON_TEST_DSN to run this example.\n";
        return 0;
    }
    auto db = halcyon::Database::open(dsn);
    if (!db.ok()) {
        std::cerr << "open failed: " << db.error().message << "\n";
        return 1;
    }
    auto r = halcyon::coro::syncWait(orders_flow(db.value()));
    if (!r.ok()) {
        std::cerr << "flow failed: " << r.error().message << "\n";
        return 1;
    }
    std::cout << "streamed " << r.value() << " rows total\n";
    return 0;
}
