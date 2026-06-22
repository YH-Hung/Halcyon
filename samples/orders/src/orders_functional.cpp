// Halcyon orders sample — functional / Result<T> style.
// Runs the SAME seven steps as orders_oo.cpp using the free-function API and
// explicit .ok()/.error() checks (no exceptions).
#include <cstdint>
#include <iostream>
#include <string>
#include <tuple>

#include "orders_model.hpp"

namespace {
// Print an error and return a non-zero exit code.
int fail(const halcyon::Error& e) {
    std::cerr << "halcyon error: " << e.message
              << " (sqlstate=" << e.sqlstate << ")\n";
    return 1;
}
}  // namespace

int main() {
    const std::string dsn = resolveDsn();

    // 1. Open the database.
    auto dbr = halcyon::Database::open(dsn);
    if (!dbr.ok()) return fail(dbr.error());
    halcyon::Database db = std::move(dbr.value());
    std::cout << "== Connected ==\n";

    // Make the run idempotent.
    if (auto r = halcyon::execute(db, sql::kResetDemoRows); !r.ok())
        return fail(r.error());

    // 2. Query all customers into a typed struct vector.
    std::cout << "\n== Customers ==\n";
    auto customers = halcyon::query_as<Customer>(db, sql::kSelectCustomers);
    if (!customers.ok()) return fail(customers.error());
    for (const auto& c : customers.value()) printCustomer(c);

    // 3. Query one customer's orders using a NAMED parameter.
    std::cout << "\n== Orders for customer #1 ==\n";
    auto orders = halcyon::query_as<Order>(db, sql::kSelectOrdersForCustomer,
                                           halcyon::params{{"cid", 1}});
    if (!orders.ok()) return fail(orders.error());
    for (const auto& o : orders.value()) printOrder(o);

    // 4. Insert one order with POSITIONAL binds (Decimal + Timestamp).
    auto inserted = halcyon::execute(
        db, sql::kInsertOrder, std::string("ORD-2001"), 1, std::string("NEW"),
        halcyon::Decimal("129.99"), halcyon::Timestamp{"2026-06-22 09:30:00"});
    if (!inserted.ok()) return fail(inserted.error());
    std::cout << "\n== Inserted " << inserted.value() << " order ==\n";

    // 5. Batch-insert two more orders (tuple rows).
    auto batch = halcyon::batchOf({
        std::make_tuple(std::string("ORD-2002"), 2, std::string("NEW"),
                        halcyon::Decimal("59.90"),
                        halcyon::Timestamp{"2026-06-22 09:35:00"}),
        std::make_tuple(std::string("ORD-2003"), 3, std::string("NEW"),
                        halcyon::Decimal("249.00"),
                        halcyon::Timestamp{"2026-06-22 09:40:00"}),
    });
    auto batched = db.executeBatch(sql::kInsertOrder, batch);
    if (!batched.ok()) return fail(batched.error());
    std::cout << "== Batch inserted " << batched.value() << " orders ==\n";

    // 6. Update two orders atomically inside a transaction. Returning an error
    //    Result from the lambda rolls back instead of committing.
    auto updated = halcyon::transaction(
        db, [](halcyon::Transaction& tx) -> halcyon::Result<std::int64_t> {
            auto a = tx.execute(sql::kUpdateOrderStatus, std::string("SHIPPED"),
                                std::string("ORD-2001"));
            if (!a.ok()) return a.error();
            auto b = tx.execute(sql::kUpdateOrderStatus, std::string("PAID"),
                                std::string("ORD-2002"));
            if (!b.ok()) return b.error();
            return a.value() + b.value();
        });
    if (!updated.ok()) return fail(updated.error());
    std::cout << "== Transaction updated " << updated.value() << " orders ==\n";

    // 7. Re-query to prove the update landed (read-after-write).
    std::cout << "\n== ORD-2001 after update ==\n";
    auto after = halcyon::query_as<Order>(db, sql::kSelectOrderByNo,
                                          std::string("ORD-2001"));
    if (!after.ok()) return fail(after.error());
    for (const auto& o : after.value()) printOrder(o);

    std::cout << "\n== Done ==\n";
    return 0;
}
