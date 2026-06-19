// Functional/Result style with a transaction.
#include <cstdint>
#include <cstdlib>
#include <iostream>

#include "halcyon/halcyon.hpp"

int main() {
    const char* dsn = std::getenv("HALCYON_TEST_DSN");
    if (!dsn) {
        std::cout << "Set HALCYON_TEST_DSN to run this example.\n";
        return 0;
    }
    auto driver = halcyon::detail::cli::make_db2_cli_driver();
    auto db = halcyon::Database::open(*driver, dsn);
    if (!db.ok()) {
        std::cerr << "open failed: " << db.error().message << "\n";
        return 1;
    }
    auto r = halcyon::transaction(
        db.value(),
        [](halcyon::Transaction& tx) -> halcyon::Result<std::int64_t> {
            return tx.execute("SELECT 1 FROM SYSIBM.SYSDUMMY1");
        });
    if (!r.ok()) {
        std::cerr << "txn failed: " << r.error().message << "\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
