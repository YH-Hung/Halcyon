// OO/throwing style. Requires a live DSN in HALCYON_TEST_DSN to actually run;
// otherwise it prints usage and exits 0 so the build stays self-contained.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "halcyon/halcyon.hpp"

struct User {
    std::int64_t id;
    std::string name;
};
HALCYON_REFLECT(User, id, name);

int main() {
    const char* dsn = std::getenv("HALCYON_TEST_DSN");
    if (!dsn) {
        std::cout << "Set HALCYON_TEST_DSN to run this example.\n";
        return 0;
    }
    try {
        auto db = halcyon::Database::openOrThrow(dsn, halcyon::PoolConfig{});
        auto n = db.executeOrThrow("SELECT 1 FROM SYSIBM.SYSDUMMY1");
        (void)n;
        auto rows = db.queryOrThrow("SELECT 1 FROM SYSIBM.SYSDUMMY1");
        for (auto& row : rows) {
            std::cout << "value=" << std::get<0>(row.as<std::int64_t>()) << "\n";
        }
    } catch (const halcyon::Exception& e) {
        std::cerr << "halcyon error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
