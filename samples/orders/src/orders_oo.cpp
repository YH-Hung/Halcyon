// Temporary connectivity smoke; replaced by the full walkthrough in Task 5.
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <halcyon/halcyon.hpp>

int main() {
    const char* env = std::getenv("HALCYON_TEST_DSN");
    const std::string dsn =
        env ? env
            : "DATABASE=SAMPLE;HOSTNAME=localhost;PORT=50000;UID=db2inst1;PWD=halcyon;";
    try {
        auto db = halcyon::Database::openOrThrow(dsn);
        auto rows = db.queryOrThrow("SELECT 1 FROM SYSIBM.SYSDUMMY1");
        for (auto& row : rows) {
            std::cout << "connected, value="
                      << std::get<0>(row.as<std::int64_t>()) << "\n";
        }
    } catch (const halcyon::Exception& e) {
        std::cerr << "halcyon error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
