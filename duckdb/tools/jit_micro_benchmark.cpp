//===----------------------------------------------------------------------===//
// JIT micro-benchmark (CSCI 543 – Project 2)
//
// Stress-tests the JIT dispatcher / threshold on synthetic arithmetic-heavy SQL:
//   1) Pure arithmetic projection
//   2) Arithmetic filter
//   3) Mixed filter + projection
//
// Build (from duckdb/):
//   cmake -B build -DCMAKE_BUILD_TYPE=Release -DDUCKDB_JIT_ENABLED=ON ...
//   cmake --build build --target jit_micro_benchmark -j
//
// Run:
//   ./build/tools/jit_micro_benchmark [rows] [repeat] [threshold] [csv_out]
//
//   rows      – rows in table t (default: 1_000_000). Each query scans this many rows per run.
//   repeat    – how many times each query is executed (default: 1). Use large values (e.g. 1_000_000)
//               with a *small* row count to stress dispatch / threshold over many identical queries.
//   threshold – JIT compilation tuple threshold (default: 1000)
//   csv_out   – optional CSV path (default: jit_micro_results.csv)
//
// Optional: DUCKDB_BENCHMARK_THREADS=N  (same as other project benchmarks)
// Optional: JIT_LOG_COMPILE=1   — print per-expression compile / reject lines from JITDispatcher
//===----------------------------------------------------------------------===//

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/execution/jit/jit_cache.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace duckdb;

namespace {

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
		std::cerr << "  [ERROR] " << res->GetError() << "\n";
		return -1.0;
	}
	if (res->type == QueryResultType::STREAM_RESULT) {
		auto mat = unique_ptr_cast<QueryResult, MaterializedQueryResult>(std::move(res));
	}
	auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
	return us / 1000.0;
}

//! One warm-up + timed runs; returns average ms per query execution.
static double BenchmarkQuery(Connection &conn, const std::string &sql, idx_t repeat) {
	MeasureMs(conn, sql);
	double total = 0.0;
	idx_t valid = 0;
	for (idx_t i = 0; i < repeat; ++i) {
		double ms = MeasureMs(conn, sql);
		if (ms >= 0.0) {
			total += ms;
			++valid;
		}
	}
	return valid > 0 ? total / static_cast<double>(valid) : -1.0;
}

struct Scenario {
	const char *name;
	const char *sql;
};

} // namespace

int main(int argc, char *argv[]) {
	const idx_t row_count = (argc > 1) ? static_cast<idx_t>(std::stoull(argv[1])) : 1000000ULL;
	const idx_t repeat = (argc > 2) ? static_cast<idx_t>(std::stoull(argv[2])) : 1ULL;
	const idx_t jit_threshold = (argc > 3) ? static_cast<idx_t>(std::stoull(argv[3])) : 1000ULL;
	const std::string csv_out = (argc > 4) ? argv[4] : "jit_micro_results.csv";

	std::cout << "\n"
	          << "========================================================================\n"
	          << "  JIT micro-benchmark (arithmetic projection / filter / mixed)\n"
	          << "========================================================================\n"
	          << "  Table rows        : " << row_count << "\n"
	          << "  Repeat per query  : " << repeat << " (each scenario runs this many times)\n"
	          << "  JIT threshold     : " << jit_threshold << " tuples\n"
	          << "  Output CSV        : " << csv_out << "\n\n";

	DuckDB db(nullptr);
	Connection conn(db);
	ApplyBenchmarkThreadCap(conn);

	auto &dispatcher = JITDispatcher::GetInstance();
	auto &cache = JITCache::GetInstance();

	dispatcher.SetCompilationThreshold(jit_threshold);
	cache.SetMaxEntries(256);

	// Deterministic INT64 columns — matches JITCompiler supported physical types.
	std::cout << "Creating table t(" << row_count << " rows) …\n";
	{
		std::ostringstream setup;
		setup << "DROP TABLE IF EXISTS t; "
		      << "CREATE TABLE t AS "
		      << "SELECT (i % 200)::BIGINT AS a, ((i * 7) % 150)::BIGINT AS b, ((i * 13) % 120)::BIGINT AS c "
		      << "FROM range(" << row_count << ") AS r(i);";
		auto res = conn.Query(setup.str());
		if (res->HasError()) {
			std::cerr << "ERROR: failed to create t: " << res->GetError() << "\n";
			return 1;
		}
	}
	std::cout << "Done.\n\n";

	const Scenario scenarios[] = {
	    {"projection",
	     "SELECT a * b + c, a / (b + 1.0), a * 0.9 - c * 0.1 FROM t"},
	    {"filter", "SELECT * FROM t WHERE a * 1.1 > 50 AND b * c < 1000000"},
	    {"mixed", "SELECT a * b + c FROM t WHERE a * 1.1 > 50"},
	};

	std::vector<std::string> names;
	std::vector<double> vec_ms;
	std::vector<double> jit_ms;

	for (const auto &sc : scenarios) {
		std::cout << "--- " << sc.name << " ---\n"
		          << sc.sql << "\n";

		dispatcher.ResetStats();
		dispatcher.SetEnableJIT(false);
		JITProfiler::GetInstance().Reset();
		cache.Clear();
		double v = BenchmarkQuery(conn, sc.sql, repeat);

		dispatcher.ResetStats();
		dispatcher.SetEnableJIT(true);
		JITProfiler::GetInstance().Reset();
		cache.Clear();
		double j = BenchmarkQuery(conn, sc.sql, repeat);

		names.push_back(sc.name);
		vec_ms.push_back(v);
		jit_ms.push_back(j);

		double speedup = (v > 0 && j > 0) ? (v / j) : 0.0;
		std::cout << "  Vector avg: " << std::fixed << std::setprecision(3) << v << " ms\n"
		          << "  JIT avg:    " << j << " ms\n"
		          << "  Speedup:    " << std::setprecision(2) << speedup << "x\n";
		std::cout << "\n  JITDispatcher stats (" << sc.name << "):\n";
		dispatcher.PrintStats();
		std::cout << "\n";
	}

	{
		std::ofstream f(csv_out);
		if (!f.is_open()) {
			std::cerr << "WARNING: could not write " << csv_out << "\n";
		} else {
			f << "scenario,rows,repeat,threshold,vectorized_avg_ms,jit_avg_ms,speedup\n";
			for (size_t i = 0; i < names.size(); ++i) {
				double sp = (vec_ms[i] > 0 && jit_ms[i] > 0) ? (vec_ms[i] / jit_ms[i]) : 0.0;
				f << names[i] << "," << row_count << "," << repeat << "," << jit_threshold << "," << vec_ms[i] << ","
				  << jit_ms[i] << "," << sp << "\n";
			}
			std::cout << "Results written to: " << csv_out << "\n";
		}
	}

	return 0;
}
