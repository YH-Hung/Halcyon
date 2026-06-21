#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "halcyon/database.hpp"
#include "halcyon/detail/cli/db2_cli_driver.hpp"
#include "concurrent_fake_driver.hpp"
#include "workloads.hpp"
#include "workload_runner.hpp"

using namespace halcyon;
using namespace halcyon::stress;

namespace {

struct Options {
    std::string backend = "fake";
    std::vector<std::string> scenarios = {"all"};
    std::vector<std::size_t> threads = {1, 2, 4, 8, 16};
    std::size_t pool_max = 8;
    std::chrono::milliseconds duration{5000};
    std::chrono::milliseconds warmup{1000};
    long latency_us = 0;
    std::uint64_t seed = 0;
    std::string format = "table";
    bool strict = false;
    // Soft-gate thresholds (fake backend only; spec §7.2 — all configurable).
    double scaling_mult = 1.5;     // peak throughput >= scaling_mult * base
    double latency_mult = 50.0;    // p99 <= latency_mult * p50 (tail sanity)
    double starvation_max = 0.25;  // tolerated error-rate ceiling
};

std::vector<std::string> split_csv(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ','))
        if (!tok.empty()) out.push_back(tok);
    return out;
}

std::vector<std::size_t> parse_threads(const std::string& csv) {
    std::vector<std::size_t> out;
    for (const auto& tok : split_csv(csv))
        out.push_back(static_cast<std::size_t>(std::stoul(tok)));
    return out;
}

bool arg_val(const std::string& a, const char* key, std::string& out) {
    const std::string prefix = std::string(key) + "=";
    if (a.rfind(prefix, 0) == 0) { out = a.substr(prefix.size()); return true; }
    return false;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i], v;
        if (arg_val(a, "--backend", v)) o.backend = v;
        else if (arg_val(a, "--scenario", v)) o.scenarios = split_csv(v);
        else if (arg_val(a, "--threads", v)) o.threads = parse_threads(v);
        else if (arg_val(a, "--pool-max", v)) o.pool_max = std::stoul(v);
        else if (arg_val(a, "--duration", v)) o.duration = std::chrono::milliseconds(std::stol(v));
        else if (arg_val(a, "--warmup", v)) o.warmup = std::chrono::milliseconds(std::stol(v));
        else if (arg_val(a, "--latency", v)) o.latency_us = std::stol(v);
        else if (arg_val(a, "--seed", v)) o.seed = std::stoull(v);
        else if (arg_val(a, "--format", v)) o.format = v;
        else if (arg_val(a, "--scaling-mult", v)) o.scaling_mult = std::stod(v);
        else if (arg_val(a, "--latency-mult", v)) o.latency_mult = std::stod(v);
        else if (arg_val(a, "--starvation-max", v)) o.starvation_max = std::stod(v);
        else if (a == "--strict") o.strict = true;
        else { std::cerr << "unknown arg: " << a << "\n"; std::exit(2); }
    }
    return o;
}

struct ScenarioSpec { ScenarioId id; const char* name; };
const std::vector<ScenarioSpec> kScenarios = {
    {ScenarioId::Pool, "pool"},   {ScenarioId::Executor, "executor"},
    {ScenarioId::Cache, "cache"}, {ScenarioId::Reconnect, "reconnect"},
    {ScenarioId::Txn, "txn"},     {ScenarioId::Lifecycle, "lifecycle"},
};

Workload make_workload(ScenarioId id, Database& db) {
    switch (id) {
        case ScenarioId::Pool: return make_pool_contention(db);
        case ScenarioId::Executor: return make_executor_saturation(db);
        case ScenarioId::Cache: return make_cache_churn(db);
        case ScenarioId::Reconnect: return make_reconnect_faults(db);
        case ScenarioId::Txn: return make_txn_churn(db);
        case ScenarioId::Lifecycle: return make_pool_contention(db);
    }
    return make_pool_contention(db);
}

}  // namespace

int main(int argc, char** argv) {
    Options o = parse(argc, argv);

    std::optional<std::string> dsn;
    if (o.backend == "live") {
        const char* env = std::getenv("HALCYON_TEST_DSN");
        if (!env) {
            std::cerr << "--backend=live requires HALCYON_TEST_DSN to be set\n";
            return 2;
        }
        dsn = env;
    } else if (o.backend != "fake") {
        std::cerr << "--backend must be fake or live\n";
        return 2;
    }

    // A scenario is selected if --scenario contains "all" or its exact name.
    auto wanted = [&](const char* name) {
        for (const auto& s : o.scenarios)
            if (s == "all" || s == name) return true;
        return false;
    };

    const bool csv = (o.format == "csv");
    std::cout << (csv ? "scenario,threads,pool_max,throughput_ops_s,p50_us,p95_us,"
                        "p99_us,max_us,ops,errors\n"
                      : "scenario        threads pool  thr(ops/s)   p50us   p99us"
                        "   maxus      ops      err\n");

    int exit_code = 0;
    for (const auto& sc : kScenarios) {
        if (!wanted(sc.name)) continue;
        // Reconnect's fault injection is fake-only; skip it on the live backend.
        if (o.backend == "live" && sc.id == ScenarioId::Reconnect) continue;

        double thr_at_min = 0.0, thr_peak = 0.0;
        double worst_lat_ratio = 0.0, worst_err_rate = 0.0;
        for (std::size_t t : o.threads) {
            // Build a fresh backend per cell so counters/caches start clean.
            std::shared_ptr<ConcurrentFakeDriver> fake;
            Result<Database> dbr = [&] {
                PoolConfig cfg = config_for(sc.id, o.pool_max);
                if (o.backend == "live")
                    return Database::open(*dsn, cfg);
                fake = std::make_shared<ConcurrentFakeDriver>();
                fake->queryLatencyUs = o.latency_us;
                if (sc.id == ScenarioId::Reconnect) {
                    fake->failExecuteEvery = 7;
                    fake->killConnectionEvery = 11;
                }
                return Database::open(
                    std::static_pointer_cast<detail::cli::ICliDriver>(fake),
                    "dsn", cfg);
            }();
            if (!dbr.ok()) {
                std::cerr << "open failed for " << sc.name << ": "
                          << dbr.error().message << "\n";
                exit_code = 1;
                continue;
            }
            Database& db = dbr.value();

            Workload w = make_workload(sc.id, db);
            RunConfig cfg;
            cfg.threads = t;
            cfg.stop.duration = o.duration;
            cfg.warmup_duration = o.warmup;  // discarded before timing (spec §5.4)
            cfg.seed = o.seed;
            RunReport r = run_workload(w, cfg);

            const double thr = r.throughput_per_sec();
            if (t == o.threads.front()) thr_at_min = thr;
            if (thr > thr_peak) thr_peak = thr;
            if (r.p50_us() > 0.0)
                worst_lat_ratio =
                    std::max(worst_lat_ratio, r.p99_us() / r.p50_us());
            worst_err_rate = std::max(worst_err_rate, r.error_rate());

            if (csv) {
                std::cout << sc.name << ',' << t << ',' << o.pool_max << ','
                          << thr << ',' << r.p50_us() << ',' << r.p95_us() << ','
                          << r.p99_us() << ',' << r.max_us() << ',' << r.ops << ','
                          << r.errors << '\n';
            } else {
                char line[256];
                std::snprintf(line, sizeof(line),
                              "%-15s %7zu %4zu %12.1f %7.1f %7.1f %8.1f %9llu %8llu",
                              sc.name, t, o.pool_max, thr, r.p50_us(), r.p99_us(),
                              r.max_us(),
                              static_cast<unsigned long long>(r.ops),
                              static_cast<unsigned long long>(r.errors));
                std::cout << line << (r.failed ? "  FAILED" : "") << "\n";
            }
            if (r.failed) exit_code = 1;
        }

        // Soft gates (fake backend only; spec §7.2). Reported as PASS/WARN; they
        // affect the exit code only under --strict so they never break CI. Live
        // numbers are dominated by DB/network latency, so gating them is noise.
        if (o.backend == "fake") {
            // Scaling: throughput rises with threads rather than serialising on a
            // mutex. Needs at least two thread counts to compare base vs peak.
            if (o.threads.size() > 1) {
                const bool ok = thr_peak >= o.scaling_mult * thr_at_min;
                std::cout << "  [gate] " << sc.name << " scaling "
                          << (ok ? "PASS" : "WARN") << " (peak " << thr_peak
                          << " vs base " << thr_at_min << " ops/s, need "
                          << o.scaling_mult << "x)\n";
                if (!ok && o.strict) exit_code = 1;
            }
            // Latency sanity: p99 within a configurable multiple of p50 (catches
            // lock-convoy / pathological tail behaviour).
            {
                const bool ok = worst_lat_ratio <= o.latency_mult;
                std::cout << "  [gate] " << sc.name << " latency "
                          << (ok ? "PASS" : "WARN") << " (worst p99/p50 "
                          << worst_lat_ratio << ", max " << o.latency_mult
                          << ")\n";
                if (!ok && o.strict) exit_code = 1;
            }
            // No starvation: tolerated error rate (mainly acquire-timeouts under
            // contention) stays below a configurable ceiling.
            {
                const bool ok = worst_err_rate <= o.starvation_max;
                std::cout << "  [gate] " << sc.name << " no-starvation "
                          << (ok ? "PASS" : "WARN") << " (worst err-rate "
                          << worst_err_rate << ", max " << o.starvation_max
                          << ")\n";
                if (!ok && o.strict) exit_code = 1;
            }
        }
    }
    return exit_code;
}
