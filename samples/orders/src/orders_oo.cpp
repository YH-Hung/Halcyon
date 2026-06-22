// Halcyon orders sample — object-oriented / throwing style.
// Runs the SAME seven steps as orders_functional.cpp; diff the two files to see
// how the throwing API compares to the Result<T> API.
#include <cstdint>
#include <iostream>
#include <string>
#include <tuple>

#include "orders_model.hpp"

int main() {
    const std::string dsn = resolveDsn();
    try {
        // 1. Open the database.
        auto db = halcyon::Database::openOrThrow(dsn);
        std::cout << "== Connected ==\n";

        // Make the run idempotent: drop any rows a previous run inserted.
        db.executeOrThrow(sql::kResetDemoRows);

        // 2. Query all customers, mapped into a typed struct vector.
        std::cout << "\n== Customers ==\n";
        auto customers = db.queryAsOrThrow<Customer>(sql::kSelectCustomers);
        for (const auto& c : customers) printCustomer(c);

        // 3. Query one customer's orders using a NAMED parameter.
        std::cout << "\n== Orders for customer #1 ==\n";
        auto orders = db.queryAsOrThrow<Order>(
            sql::kSelectOrdersForCustomer, halcyon::params{{"cid", 1}});
        for (const auto& o : orders) printOrder(o);

        // 4. Insert one order with POSITIONAL binds (Decimal + Timestamp).
        auto inserted = db.executeOrThrow(
            sql::kInsertOrder, std::string("ORD-2001"), 1, std::string("NEW"),
            halcyon::Decimal("129.99"),
            halcyon::Timestamp{"2026-06-22 09:30:00"});
        std::cout << "\n== Inserted " << inserted << " order ==\n";

        // 5. Batch-insert two more orders (tuple rows).
        auto batch = halcyon::batchOf({
            std::make_tuple(std::string("ORD-2002"), 2, std::string("NEW"),
                            halcyon::Decimal("59.90"),
                            halcyon::Timestamp{"2026-06-22 09:35:00"}),
            std::make_tuple(std::string("ORD-2003"), 3, std::string("NEW"),
                            halcyon::Decimal("249.00"),
                            halcyon::Timestamp{"2026-06-22 09:40:00"}),
        });
        auto batched = db.executeBatch(sql::kInsertOrder, batch).value();
        std::cout << "== Batch inserted " << batched << " orders ==\n";

        // 6. Update two orders atomically inside a transaction. Returning an
        //    error Result from the lambda would roll back instead of commit.
        auto updated = db.transaction(
            [](halcyon::Transaction& tx) -> halcyon::Result<std::int64_t> {
                auto a = tx.execute(sql::kUpdateOrderStatus,
                                    std::string("SHIPPED"),
                                    std::string("ORD-2001"));
                if (!a.ok()) return a.error();
                auto b = tx.execute(sql::kUpdateOrderStatus,
                                    std::string("PAID"),
                                    std::string("ORD-2002"));
                if (!b.ok()) return b.error();
                return a.value() + b.value();
            });
        std::cout << "== Transaction updated " << updated.value()
                  << " orders ==\n";

        // 7. Re-query to prove the update landed (read-after-write).
        std::cout << "\n== ORD-2001 after update ==\n";
        auto after = db.queryAsOrThrow<Order>(sql::kSelectOrderByNo,
                                              std::string("ORD-2001"));
        for (const auto& o : after) printOrder(o);

        std::cout << "\n== Done ==\n";
    } catch (const halcyon::Exception& e) {
        std::cerr << "halcyon error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
