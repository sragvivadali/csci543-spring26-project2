//===----------------------------------------------------------------------===//
// JIT micro-benchmark (CSCI 543 – Project 2)
//
// Stress-tests the JIT dispatcher / threshold on synthetic SQL over table t(a–e BIGINT):
//   Baselines: projection, filter, mixed, complex (fat arithmetic — usually strong JIT).
//   Edge cases: many_tiny (many cheap roots — dispatch + wrapper overhead),
//   tiny_heavy (LIMIT subquery — small chunks, low JIT usage until hot),
//   case_expr (CASE — compare speedup vs pure arithmetic),
//   between_filter (BETWEEN — often no JITDispatcher compile/execute on this path),
//   branchy_or (OR + AND — more branchy predicate than plain AND filter).
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

struct ScenarioSummary {
	std::string name;
	double jit_usage_pct {0.0};
	idx_t compile_attempts {0};
	idx_t compile_successes {0};
	idx_t compile_failures {0};
	double vector_avg_ms {0.0};
	double jit_avg_ms {0.0};
	double speedup {0.0};
};

static void PrintSummaryTable(const std::vector<ScenarioSummary> &rows) {
	std::cout
	    << "\n========================================================================\n"
	    << "  Summary (per scenario, after JIT phase)\n"
	    << "========================================================================\n"
	    << "  jit_pct         = JIT executions / (JIT + interpreter) from JITDispatcher\n"
	    << "  cmp_att/ok/fail = compile attempts / LLVM successes / CanCompile or null fails\n"
	    << "  vec_ms / jit_ms = mean end-to-end query latency (vectorized vs JIT path)\n"
	    << "------------------------------------------------------------------------\n";

	const int w_sc = 16;
	const int w_pct = 9;
	const int w_cmp = 9; // ≥ len("cmp_fail") so headers do not visually merge
	const int w_ms = 10;
	const int w_sp = 8;
	std::cout << std::left << std::setw(w_sc) << "scenario" << std::right << std::setw(w_pct) << "jit_pct"
	          << std::setw(w_cmp) << "cmp_att" << std::setw(w_cmp) << "cmp_ok" << std::setw(w_cmp) << "cmp_fail"
	          << std::setw(w_ms) << "vec_ms" << std::setw(w_ms) << "jit_ms" << std::setw(w_sp) << "speedup" << '\n';
	std::cout << std::string(static_cast<size_t>(w_sc + w_pct + 3 * w_cmp + 2 * w_ms + w_sp), '-') << '\n';
	std::cout << std::fixed;
	for (const auto &r : rows) {
		std::cout << std::left << std::setw(w_sc) << r.name << std::right << std::fixed << std::setprecision(2)
		          << std::setw(w_pct) << r.jit_usage_pct << std::setprecision(0) << std::setw(w_cmp)
		          << static_cast<double>(r.compile_attempts) << std::setw(w_cmp)
		          << static_cast<double>(r.compile_successes) << std::setw(w_cmp)
		          << static_cast<double>(r.compile_failures) << std::setprecision(3) << std::setw(w_ms)
		          << r.vector_avg_ms << std::setw(w_ms) << r.jit_avg_ms << std::setprecision(2) << std::setw(w_sp)
		          << r.speedup << '\n';
	}
	std::cout << "========================================================================\n";
}

} // namespace

int main(int argc, char *argv[]) {
	const idx_t row_count = (argc > 1) ? static_cast<idx_t>(std::stoull(argv[1])) : 1000000ULL;
	const idx_t repeat = (argc > 2) ? static_cast<idx_t>(std::stoull(argv[2])) : 1ULL;
	const idx_t jit_threshold = (argc > 3) ? static_cast<idx_t>(std::stoull(argv[3])) : 1000ULL;
	const std::string csv_out = (argc > 4) ? argv[4] : "jit_micro_results.csv";

	std::cout << "\n"
	          << "========================================================================\n"
	          << "  JIT micro-benchmark (synthetic scenarios + edge cases)\n"
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
	// Enough slots for many_tiny (many distinct expression roots) without LRU churn.
	cache.SetMaxEntries(512);

	// Deterministic INT64 columns — matches JITCompiler supported physical types.
	std::cout << "Creating table t(" << row_count << " rows) …\n";
	{
		std::ostringstream setup;
		setup << "DROP TABLE IF EXISTS t; "
		      << "CREATE TABLE t AS "
		      << "SELECT (i % 200)::BIGINT AS a, "
		      << "((i * 7) % 150)::BIGINT AS b, "
		      << "((i * 13) % 120)::BIGINT AS c, "
		      << "((i * 11) % 100)::BIGINT AS d, "
		      << "((i * 3) % 80)::BIGINT AS e "
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
	    {"complex",
	     "SELECT (a * b + c * d) / (e + 1.0) - (a * 0.9 + b * 0.1) FROM t"},
	    // Many cheap roots: vectorized wins on breadth; JIT pays per-root wrapper + compile.
	    {"many_tiny",
	     "SELECT a + 1, b + 1, c + 1, d + 1, e + 1, a + b, b + c, c + d, d + e, a - b, b - c, c - d FROM t"},
	    // Same heavy kernel as complex, but tiny input: fixed compile/dispatch cost dominates.
	    {"tiny_heavy",
	     "SELECT (a * b + c * d) / (e + 1.0) - (a * 0.9 + b * 0.1) FROM (SELECT * FROM t LIMIT 8192) s"},
	    {"case_expr", "SELECT CASE WHEN a > 100 THEN b + c ELSE d + e END FROM t"},
	    {"between_filter", "SELECT a FROM t WHERE a BETWEEN 10 AND 190"},
	    {"branchy_or", "SELECT * FROM t WHERE (a > 150 OR b < 20) AND c > 10 AND d > e"},
	};

	std::vector<std::string> names;
	std::vector<double> vec_ms;
	std::vector<double> jit_ms;
	std::vector<ScenarioSummary> summary_rows;

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

		ScenarioSummary sum;
		sum.name = sc.name;
		sum.vector_avg_ms = v;
		sum.jit_avg_ms = j;
		sum.speedup = (v > 0 && j > 0) ? (v / j) : 0.0;
		const auto dst = dispatcher.GetStats();
		const idx_t exec_total = dst.jit_executions + dst.interpreter_executions;
		sum.jit_usage_pct =
		    exec_total > 0 ? (100.0 * static_cast<double>(dst.jit_executions) / static_cast<double>(exec_total)) : 0.0;
		sum.compile_attempts = dst.compilation_attempts;
		sum.compile_successes = dst.compilation_successes;
		sum.compile_failures = dst.compilation_failures;
		summary_rows.push_back(std::move(sum));

		double speedup = (v > 0 && j > 0) ? (v / j) : 0.0;
		std::cout << "  Vector avg: " << std::fixed << std::setprecision(3) << v << " ms\n"
		          << "  JIT avg:    " << j << " ms\n"
		          << "  Speedup:    " << std::setprecision(2) << speedup << "x\n";
		std::cout << "\n  JITDispatcher stats (" << sc.name << "):\n";
		dispatcher.PrintStats();
		std::cout << "\n";
	}

	PrintSummaryTable(summary_rows);

	{
		std::ofstream f(csv_out);
		if (!f.is_open()) {
			std::cerr << "WARNING: could not write " << csv_out << "\n";
		} else {
			f << "scenario,rows,repeat,threshold,jit_usage_pct,compile_attempts,compile_successes,compile_failures,"
			     "vectorized_avg_ms,jit_avg_ms,speedup\n";
			for (size_t i = 0; i < names.size(); ++i) {
				double sp = (vec_ms[i] > 0 && jit_ms[i] > 0) ? (vec_ms[i] / jit_ms[i]) : 0.0;
				const auto &s = summary_rows[i];
				f << names[i] << "," << row_count << "," << repeat << "," << jit_threshold << "," << s.jit_usage_pct
				  << "," << s.compile_attempts << "," << s.compile_successes << "," << s.compile_failures << ","
				  << vec_ms[i] << "," << jit_ms[i] << "," << sp << "\n";
			}
			std::cout << "Results written to: " << csv_out << "\n";
		}
	}

	return 0;
}
