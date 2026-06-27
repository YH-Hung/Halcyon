// Live batch-insert throughput benchmark (spec §8/§9). Requires HALCYON_TEST_DSN.
// Times inserting N rows three ways and prints rows/sec:
//   1) per-row execute() under autocommit
//   2) executeBatch (array binding) under autocommit
//   3) executeBatch wrapped in a transaction (one commit)
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include "halcyon/halcyon.hpp"

namespace {
double secs_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
}
void recreate(halcyon::Database& h) {
    h.execute("DROP TABLE halcyon_bench");
    h.executeOrThrow(
        "CREATE TABLE halcyon_bench(id BIGINT NOT NULL, payload VARCHAR(64))");
}
}  // namespace

int main() {
    const char* dsn = std::getenv("HALCYON_TEST_DSN");
    if (!dsn) {
        std::cerr << "HALCYON_TEST_DSN not set; skipping benchmark\n";
        return 0;
    }
    auto h = halcyon::Database::openOrThrow(std::string(dsn));
    const std::int64_t N = 50000;
    const std::string payload(48, 'x');

    std::vector<std::tuple<std::int64_t, std::string>> rows;
    rows.reserve(N);
    for (std::int64_t i = 0; i < N; ++i) rows.emplace_back(i, payload);
    const char* kSql = "INSERT INTO halcyon_bench(id,payload) VALUES (?,?)";

    // 1) per-row execute under autocommit
    recreate(h);
    auto t0 = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < N; ++i)
        h.executeOrThrow(kSql, i, payload);
    double perRow = secs_since(t0);

    // 2) array binding under autocommit
    recreate(h);
    t0 = std::chrono::steady_clock::now();
    h.executeBatch(kSql, halcyon::batchOf(rows)).value();
    double array = secs_since(t0);

    // 3) array binding inside one transaction
    recreate(h);
    t0 = std::chrono::steady_clock::now();
    {
        auto tx = h.begin().value();
        tx.executeBatch(kSql, halcyon::batchOf(rows)).value();
        tx.commit().value();
    }
    double arrayTxn = secs_since(t0);

    h.execute("DROP TABLE halcyon_bench");

    auto rps = [](double s) { return s > 0 ? static_cast<double>(N) / s : 0; };
    std::cout << "rows=" << N << "\n"
              << "per_row_autocommit : " << perRow << "s  "
              << rps(perRow) << " rows/s\n"
              << "array_autocommit   : " << array << "s  "
              << rps(array) << " rows/s\n"
              << "array_transaction  : " << arrayTxn << "s  "
              << rps(arrayTxn) << " rows/s\n";
    return 0;
}
