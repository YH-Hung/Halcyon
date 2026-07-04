// Live read-throughput benchmark (spec §7/§8). Requires HALCYON_TEST_DSN.
// Seeds N rows of bounded columns, then times materializing them through the
// block-fetch read path (queryAs). Prints rows/sec for a narrow (2-col) and a
// wider (8-col) row shape — the per-cell-overhead saving scales with column
// count, so the wide shape shows the larger win. Baseline (the pre-block
// row-at-a-time path) is captured by building this binary from the commit before
// the read-path rewire; see the plan's Task 5 / §8.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

#include "halcyon/halcyon.hpp"

namespace {
struct Narrow {
    std::int64_t id;
    std::string name;
};
struct Wide {
    std::int64_t id;
    std::string c1, c2, c3;
    std::int64_t n1, n2, n3;
};

double secs_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
}
double rps(std::int64_t n, double s) {
    return s > 0 ? static_cast<double>(n) / s : 0;
}
}  // namespace
HALCYON_REFLECT(Narrow, id, name);
HALCYON_REFLECT(Wide, id, c1, c2, c3, n1, n2, n3);

int main() {
    const char* dsn = std::getenv("HALCYON_TEST_DSN");
    if (!dsn) {
        std::cerr << "HALCYON_TEST_DSN not set; skipping benchmark\n";
        return 0;
    }
    auto h = halcyon::Database::openOrThrow(std::string(dsn));
    const std::int64_t N = 200000;

    // --- narrow: 2 columns ---
    h.execute("DROP TABLE halcyon_read_bench");
    h.executeOrThrow(
        "CREATE TABLE halcyon_read_bench(id BIGINT NOT NULL, name VARCHAR(32))");
    {
        std::vector<std::tuple<std::int64_t, std::string>> seed;
        seed.reserve(N);
        for (std::int64_t i = 0; i < N; ++i)
            seed.emplace_back(i, "name-" + std::to_string(i));
        h.executeBatch("INSERT INTO halcyon_read_bench(id,name) VALUES (?,?)",
                       halcyon::batchOf(seed))
            .value();
    }
    auto t0 = std::chrono::steady_clock::now();
    auto narrow = h.queryAsOrThrow<Narrow>("SELECT id, name FROM halcyon_read_bench");
    double narrowSecs = secs_since(t0);
    h.execute("DROP TABLE halcyon_read_bench");

    // --- wide: 8 columns ---
    h.execute("DROP TABLE halcyon_read_bench_w");
    h.executeOrThrow(
        "CREATE TABLE halcyon_read_bench_w(id BIGINT NOT NULL, "
        "c1 VARCHAR(24), c2 VARCHAR(24), c3 VARCHAR(24), "
        "n1 BIGINT, n2 BIGINT, n3 BIGINT)");
    {
        std::vector<std::tuple<std::int64_t, std::string, std::string,
                               std::string, std::int64_t, std::int64_t,
                               std::int64_t>>
            seed;
        seed.reserve(N);
        for (std::int64_t i = 0; i < N; ++i)
            seed.emplace_back(i, "a-" + std::to_string(i), "b-" + std::to_string(i),
                              "c-" + std::to_string(i), i, i * 2, i * 3);
        h.executeBatch(
             "INSERT INTO halcyon_read_bench_w(id,c1,c2,c3,n1,n2,n3) "
             "VALUES (?,?,?,?,?,?,?)",
             halcyon::batchOf(seed))
            .value();
    }
    t0 = std::chrono::steady_clock::now();
    auto wide = h.queryAsOrThrow<Wide>(
        "SELECT id,c1,c2,c3,n1,n2,n3 FROM halcyon_read_bench_w");
    double wideSecs = secs_since(t0);
    h.execute("DROP TABLE halcyon_read_bench_w");

    std::cout << "rows=" << N << "\n"
              << "narrow_2col : " << narrow.size() << " rows  " << narrowSecs
              << "s  " << rps(static_cast<std::int64_t>(narrow.size()), narrowSecs)
              << " rows/s\n"
              << "wide_8col   : " << wide.size() << " rows  " << wideSecs << "s  "
              << rps(static_cast<std::int64_t>(wide.size()), wideSecs)
              << " rows/s\n";
    return 0;
}
