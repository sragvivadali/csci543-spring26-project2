//===----------------------------------------------------------------------===//
// TPC-H JIT vs. Vectorization Benchmark (CSCI 543 – Project 2)
//
// Compares DuckDB's native vectorized execution (JIT disabled) against the
// LLVM-based JIT compilation extension (JIT enabled) across all 22 standard
// TPC-H queries at scale factor 0.1 (fast) or 1 (full).
//
// Build:
//   cd duckdb && make debug -j$(nproc)
//   # binary lands at: build/debug/tools/tpch_jit_benchmark
//
// Run:
//   ./build/debug/tools/tpch_jit_benchmark [sf] [iterations] [csv_out]
//
//   sf          – TPC-H scale factor (default: 0.1 for speed; use 1 for full)
//   iterations  – warm-up runs per query  (default: 3)
//   csv_out     – output CSV path         (default: tpch_jit_results.csv)
//
// Example:
//   ./build/debug/tools/tpch_jit_benchmark 1 5 results/tpch_sf1.csv
//
// Optional: DUCKDB_BENCHMARK_THREADS=N caps DuckDB query parallelism (e.g. 2).
//   DUCKDB_BENCHMARK_THREADS=2 ./build/tools/tpch_jit_benchmark
//===----------------------------------------------------------------------===//

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/execution/jit/jit_benchmark.hpp"
#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/execution/jit/jit_cache.hpp"
#include "tpch_extension.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <chrono>
#include <cstdlib>
#include <stdexcept>

using namespace duckdb;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void PrintBanner(const std::string &title) {
    std::cout << "\n" << std::string(72, '=') << "\n"
              << "  " << title << "\n"
              << std::string(72, '=') << "\n";
}

static void ApplyBenchmarkThreadCap(Connection &conn) {
    const char *env = std::getenv("DUCKDB_BENCHMARK_THREADS");
    if (!env || env[0] == '\0') {
        return;
    }
    auto res = conn.Query(string("PRAGMA threads=") + env);
    if (res->HasError()) {
        std::cerr << "WARNING: PRAGMA threads failed: " << res->GetError() << "\n";
        return;
    }
    std::cout << "DuckDB worker threads set to " << env << " (DUCKDB_BENCHMARK_THREADS)\n";
}

static double MeasureMs(Connection &conn, const std::string &sql) {
    auto t0 = std::chrono::high_resolution_clock::now();
    auto res = conn.Query(sql);
    auto t1 = std::chrono::high_resolution_clock::now();

    if (res->HasError()) {
        std::cerr << "  [WARN] Query error: " << res->GetError() << "\n";
        return -1.0;
    }
    // Materialize to include fetch time
    if (res->type == QueryResultType::STREAM_RESULT) {
        auto mat = unique_ptr_cast<QueryResult, MaterializedQueryResult>(std::move(res));
    }
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return us / 1000.0;
}

// Run `sql` N times and return the average wall-clock ms.
// The first run is always a warm-up and is excluded from the average.
static double BenchmarkQuery(Connection &conn, const std::string &sql,
                             int iterations) {
    // warm-up (not counted)
    MeasureMs(conn, sql);

    double total = 0.0;
    int valid = 0;
    for (int i = 0; i < iterations; ++i) {
        double ms = MeasureMs(conn, sql);
        if (ms >= 0.0) {
            total += ms;
            ++valid;
        }
    }
    return valid > 0 ? total / valid : -1.0;
}

// ---------------------------------------------------------------------------
// Result record
// ---------------------------------------------------------------------------

struct TpchResult {
    int query_num;
    std::string query_name;        // "Q01" … "Q22"
    double vec_ms;                 // vectorized (JIT off)
    double jit_ms;                 // JIT-compiled   (JIT on)
    double speedup;                // vec_ms / jit_ms  (>1 means JIT is faster)
    bool jit_error;                // true when JIT run failed
};

// ---------------------------------------------------------------------------
// Main benchmark driver
// ---------------------------------------------------------------------------

int main(int argc, char *argv[]) {
    // --- parse args ---
    // Usage: tpch_jit_benchmark [sf] [iterations] [csv_out] [jit_threshold]
    double sf         = (argc > 1) ? std::stod(argv[1]) : 0.1;
    int    iterations = (argc > 2) ? std::stoi(argv[2]) : 3;
    std::string csv_out = (argc > 3) ? argv[3] : "tpch_jit_results.csv";
    idx_t  jit_threshold = (argc > 4) ? (idx_t)std::stoull(argv[4]) : 1000;

    PrintBanner("TPC-H JIT vs. Vectorization Benchmark  (CSCI 543)");
    std::cout << "  Scale factor      : SF" << sf      << "\n"
              << "  Iterations        : "   << iterations << " (+ 1 warm-up)\n"
              << "  Output CSV        : "   << csv_out  << "\n"
              << "  JIT threshold     : "   << jit_threshold << " tuples\n\n";

    // -----------------------------------------------------------------------
    // 1.  Open database and load TPC-H extension
    // -----------------------------------------------------------------------
    DuckDB db(nullptr);

    // Register the built-in TPC-H extension so dbgen() is available.
    // (Statically linked when built with -DBUILD_TPCH_EXTENSION=1)
    db.LoadStaticExtension<TpchExtension>();

    Connection conn(db);
    ApplyBenchmarkThreadCap(conn);

    // -----------------------------------------------------------------------
    // 2.  Generate TPC-H data
    // -----------------------------------------------------------------------
    std::cout << "Generating TPC-H data at SF" << sf << " … (this may take a moment)\n";
    {
        auto res = conn.Query("CALL dbgen(sf=" + std::to_string(sf) + ")");
        if (res->HasError()) {
            std::cerr << "ERROR: dbgen failed: " << res->GetError() << "\n";
            return 1;
        }
    }
    std::cout << "Data generation complete.\n\n";

    // -----------------------------------------------------------------------
    // 3.  Configure JIT system
    // -----------------------------------------------------------------------
    auto &dispatcher = JITDispatcher::GetInstance();
    auto &profiler   = JITProfiler::GetInstance();
    auto &cache      = JITCache::GetInstance();

    // Threshold controls how many tuples an expression must see before JIT compilation fires.
    // Lower = compiles sooner (more JIT coverage, more compilation overhead).
    // Higher = compiles later (less coverage, better amortization of compile cost).
    dispatcher.SetCompilationThreshold(jit_threshold);
    cache.SetMaxEntries(256);

    // -----------------------------------------------------------------------
    // 4.  Run all 22 TPC-H queries
    // -----------------------------------------------------------------------
    std::vector<TpchResult> results;

    // Header row
    std::cout << std::left  << std::setw(6)  << "Query"
              << std::right << std::setw(14) << "Vec (ms)"
              << std::setw(14) << "JIT (ms)"
              << std::setw(10) << "Speedup"
              << "\n";
    std::cout << std::string(44, '-') << "\n";

    for (int q = 1; q <= 22; ++q) {
        std::string qname = std::string("Q") + (q < 10 ? "0" : "") + std::to_string(q);
        std::string sql;

        // Retrieve SQL from the TPC-H extension (no file I/O needed)
        try {
            sql = TpchExtension::GetQuery(q);
        } catch (std::exception &e) {
            std::cerr << qname << ": could not fetch SQL – " << e.what() << "\n";
            continue;
        }

        // -- Vectorized run (JIT disabled = DuckDB native vectorization) --
        dispatcher.SetEnableJIT(false);
        profiler.Reset();
        cache.Clear();
        double vec_ms = BenchmarkQuery(conn, sql, iterations);

        // -- JIT run (JIT enabled = LLVM-compiled expressions) --
        dispatcher.SetEnableJIT(true);
        profiler.Reset();
        cache.Clear();
        double jit_ms = BenchmarkQuery(conn, sql, iterations);

        bool jit_err = (jit_ms < 0.0);
        if (jit_err) jit_ms = vec_ms; // fall-back so speedup stays 1.0

        double speedup = (jit_ms > 0.0) ? (vec_ms / jit_ms) : 0.0;

        TpchResult r;
        r.query_num  = q;
        r.query_name = qname;
        r.vec_ms     = vec_ms;
        r.jit_ms     = jit_ms;
        r.speedup    = speedup;
        r.jit_error  = jit_err;
        results.push_back(r);

        // Print row
        std::cout << std::left  << std::setw(6)  << qname
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(14) << (vec_ms < 0 ? -1 : vec_ms)
                  << std::setw(14) << (jit_err ? -1 : jit_ms)
                  << std::setw(10) << speedup;
        if (jit_err) std::cout << "  [JIT ERROR]";
        std::cout << "\n";
    }

    // -----------------------------------------------------------------------
    // 5.  Summary
    // -----------------------------------------------------------------------
    PrintBanner("Summary");

    double total_vec = 0, total_jit = 0;
    int    faster_count = 0, slower_count = 0, neutral_count = 0;

    for (auto &r : results) {
        if (r.vec_ms > 0) total_vec += r.vec_ms;
        if (!r.jit_error && r.jit_ms > 0) total_jit += r.jit_ms;

        if      (r.speedup > 1.05) ++faster_count;
        else if (r.speedup < 0.95) ++slower_count;
        else                        ++neutral_count;
    }

    double overall_speedup = (total_jit > 0) ? total_vec / total_jit : 0.0;

    std::cout << "\n  Queries run           : " << results.size()
              << "\n  JIT faster (>5%)      : " << faster_count
              << "\n  JIT slower  (<-5%)    : " << slower_count
              << "\n  Roughly equal (±5%)   : " << neutral_count
              << "\n"
              << "\n  Total vectorized time : " << std::fixed << std::setprecision(1)
              << total_vec << " ms"
              << "\n  Total JIT time        : " << total_jit << " ms"
              << "\n  Overall speedup       : " << std::setprecision(2)
              << overall_speedup << "x"
              << "\n\n";

    // -----------------------------------------------------------------------
    // 6.  Export CSV
    // -----------------------------------------------------------------------
    {
        std::ofstream f(csv_out);
        if (!f.is_open()) {
            std::cerr << "WARNING: could not write " << csv_out << "\n";
        } else {
            f << "query,vectorized_ms,jit_ms,speedup,jit_error\n";
            for (auto &r : results) {
                f << r.query_name     << ","
                  << r.vec_ms         << ","
                  << (r.jit_error ? -1.0 : r.jit_ms) << ","
                  << r.speedup        << ","
                  << (r.jit_error ? "true" : "false") << "\n";
            }
            std::cout << "Results written to: " << csv_out << "\n";
        }
    }

    // -----------------------------------------------------------------------
    // 7.  Dispatcher stats
    // -----------------------------------------------------------------------
    PrintBanner("JIT Dispatcher Stats");
    dispatcher.PrintStats();

    return 0;
}
