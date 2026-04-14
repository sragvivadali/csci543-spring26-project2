//===----------------------------------------------------------------------===//
// TPC-DS JIT vs. Vectorization Benchmark (CSCI 543 – Project 2)
//
// Compares DuckDB's native vectorized execution (JIT disabled) against the
// LLVM-based JIT compilation extension (JIT enabled) across all standard
// TPC-DS queries at a chosen scale factor.
//
// Build:
//   cd duckdb && cmake -B build -DDUCKDB_JIT_ENABLED=ON ... && cmake --build build
//   # binary: build/tools/tpcds_jit_benchmark
//
// Run:
//   ./build/tools/tpcds_jit_benchmark [sf] [iterations] [csv_out] [thresholds...]
//
//   sf            – TPC-DS scale factor (default: 0.01 for a fast run; use 1 for full)
//   iterations    – timed runs per query after warm-up (default: 3)
//   csv_out       – output CSV path (default: tpcds_jit_results.csv)
//   thresholds    – one or more tuple thresholds before JIT compiles (optional).
//                   If omitted, runs three experiments: 100, 1000, 10000.
//   Example:  ./tpcds_jit_benchmark 0.01 3 results.csv 100 1000 10000
//
//   Optional: DUCKDB_BENCHMARK_THREADS=N caps DuckDB query parallelism (e.g. 2 on a small machine).
//   Example:  DUCKDB_BENCHMARK_THREADS=2 ./build/tools/tpcds_jit_benchmark
//===----------------------------------------------------------------------===//

#include "duckdb.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/execution/jit/jit_benchmark.hpp"
#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/execution/jit/jit_cache.hpp"
#include "dsdgen.hpp"
#include "tpcds_extension.hpp"

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

static void PrintBanner(const std::string &title) {
	std::cout << "\n" << std::string(72, '=') << "\n"
	          << "  " << title << "\n"
	          << std::string(72, '=') << "\n";
}

static double MeasureMs(Connection &conn, const std::string &sql) {
	auto t0 = std::chrono::high_resolution_clock::now();
	auto res = conn.Query(sql);
	auto t1 = std::chrono::high_resolution_clock::now();

	if (res->HasError()) {
		std::cerr << "  [WARN] Query error: " << res->GetError() << "\n";
		return -1.0;
	}
	if (res->type == QueryResultType::STREAM_RESULT) {
		auto mat = unique_ptr_cast<QueryResult, MaterializedQueryResult>(std::move(res));
	}
	auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
	return us / 1000.0;
}

static double BenchmarkQuery(Connection &conn, const std::string &sql, int iterations) {
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

struct TpcdsResult {
	int query_num;
	std::string query_name;
	double vec_ms;
	double jit_ms;
	double speedup;
	bool jit_error;
};

struct ThresholdSummary {
	idx_t threshold;
	double total_vec;
	double total_jit;
	double overall_speedup;
	int faster_count;
	int slower_count;
	int neutral_count;
	size_t queries_run;
};

static std::string QueryLabel(int q) {
	std::ostringstream oss;
	oss << "DSQ" << std::setfill('0') << std::setw(2) << q;
	return oss.str();
}

//! If DUCKDB_BENCHMARK_THREADS is set, run PRAGMA threads=<value> (limits worker threads for queries).
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

static ThresholdSummary RunAllQueries(Connection &conn, JITDispatcher &dispatcher, JITProfiler &profiler, JITCache &cache,
                                      uint32_t n_queries, int iterations, idx_t jit_threshold,
                                      std::vector<TpcdsResult> &out_results) {
	dispatcher.SetCompilationThreshold(jit_threshold);
	dispatcher.ResetStats();

	out_results.clear();

	std::cout << std::left << std::setw(8) << "Query" << std::right << std::setw(14) << "Vec (ms)" << std::setw(14) << "JIT (ms)"
	          << std::setw(10) << "Speedup"
	          << "\n";
	std::cout << std::string(46, '-') << "\n";

	for (uint32_t q = 1; q <= n_queries; ++q) {
		std::string qname = QueryLabel((int)q);
		std::string sql;
		try {
			sql = TpcdsExtension::GetQuery((int)q);
		} catch (std::exception &e) {
			std::cerr << qname << ": could not fetch SQL – " << e.what() << "\n";
			continue;
		}

		dispatcher.SetEnableJIT(false);
		profiler.Reset();
		cache.Clear();
		double vec_ms = BenchmarkQuery(conn, sql, iterations);

		dispatcher.SetEnableJIT(true);
		profiler.Reset();
		cache.Clear();
		double jit_ms = BenchmarkQuery(conn, sql, iterations);

		bool jit_err = (jit_ms < 0.0);
		if (jit_err)
			jit_ms = vec_ms;

		double speedup = (jit_ms > 0.0) ? (vec_ms / jit_ms) : 0.0;

		TpcdsResult r;
		r.query_num = (int)q;
		r.query_name = qname;
		r.vec_ms = vec_ms;
		r.jit_ms = jit_ms;
		r.speedup = speedup;
		r.jit_error = jit_err;
		out_results.push_back(r);

		std::cout << std::left << std::setw(8) << qname << std::right << std::fixed << std::setprecision(2) << std::setw(14)
		          << (vec_ms < 0 ? -1 : vec_ms) << std::setw(14) << (jit_err ? -1 : jit_ms) << std::setw(10) << speedup;
		if (jit_err)
			std::cout << "  [JIT ERROR]";
		std::cout << "\n";
	}

	ThresholdSummary s;
	s.threshold = jit_threshold;
	s.total_vec = 0;
	s.total_jit = 0;
	s.faster_count = 0;
	s.slower_count = 0;
	s.neutral_count = 0;
	s.queries_run = out_results.size();

	for (auto &r : out_results) {
		if (r.vec_ms > 0)
			s.total_vec += r.vec_ms;
		if (!r.jit_error && r.jit_ms > 0)
			s.total_jit += r.jit_ms;

		if (r.speedup > 1.05)
			++s.faster_count;
		else if (r.speedup < 0.95)
			++s.slower_count;
		else
			++s.neutral_count;
	}

	s.overall_speedup = (s.total_jit > 0) ? s.total_vec / s.total_jit : 0.0;

	return s;
}

int main(int argc, char *argv[]) {
	double sf = (argc > 1) ? std::stod(argv[1]) : 0.01;
	int iterations = (argc > 2) ? std::stoi(argv[2]) : 3;
	std::string csv_out = (argc > 3) ? argv[3] : "tpcds_jit_results.csv";

	std::vector<idx_t> thresholds;
	if (argc > 4) {
		for (int i = 4; i < argc; ++i) {
			thresholds.push_back(static_cast<idx_t>(std::stoull(argv[i])));
		}
	} else {
		thresholds = {100, 1000, 10000};
	}

	const uint32_t n_queries = tpcds::DSDGenWrapper::QueriesCount();

	PrintBanner("TPC-DS JIT vs. Vectorization Benchmark  (CSCI 543)");
	std::cout << "  Scale factor      : SF" << sf << "\n"
	          << "  Queries           : " << n_queries << "\n"
	          << "  Iterations        : " << iterations << " (+ 1 warm-up)\n"
	          << "  Output CSV        : " << csv_out << "\n"
	          << "  JIT thresholds    : ";
	for (size_t i = 0; i < thresholds.size(); ++i) {
		if (i)
			std::cout << ", ";
		std::cout << thresholds[i] << " tuples";
	}
	std::cout << "\n\n";

	DuckDB db(nullptr);
	db.LoadStaticExtension<TpcdsExtension>();

	Connection conn(db);
	ApplyBenchmarkThreadCap(conn);

	std::cout << "Generating TPC-DS data at SF" << sf << " … (this may take a while)\n";
	{
		auto res = conn.Query("CALL dsdgen(sf=" + std::to_string(sf) + ")");
		if (res->HasError()) {
			std::cerr << "ERROR: dsdgen failed: " << res->GetError() << "\n";
			return 1;
		}
	}
	std::cout << "Data generation complete.\n\n";

	auto &dispatcher = JITDispatcher::GetInstance();
	auto &profiler = JITProfiler::GetInstance();
	auto &cache = JITCache::GetInstance();

	cache.SetMaxEntries(256);

	std::vector<ThresholdSummary> all_summaries;
	std::vector<std::pair<idx_t, std::vector<TpcdsResult>>> all_rows;

	for (idx_t jit_threshold : thresholds) {
		std::ostringstream oss;
		oss << "Threshold = " << jit_threshold << " tuples";
		PrintBanner(oss.str());

		std::vector<TpcdsResult> results;
		ThresholdSummary s = RunAllQueries(conn, dispatcher, profiler, cache, n_queries, iterations, jit_threshold, results);
		all_summaries.push_back(s);
		all_rows.emplace_back(jit_threshold, std::move(results));

		std::cout << "\n  Queries run           : " << s.queries_run << "\n  JIT faster (>5%)      : " << s.faster_count
		          << "\n  JIT slower  (<-5%)    : " << s.slower_count << "\n  Roughly equal (±5%)   : " << s.neutral_count
		          << "\n"
		          << "\n  Total vectorized time : " << std::fixed << std::setprecision(1) << s.total_vec << " ms"
		          << "\n  Total JIT time        : " << s.total_jit << " ms"
		          << "\n  Overall speedup       : " << std::setprecision(2) << s.overall_speedup << "x"
		          << "\n\n";

		PrintBanner("JIT Dispatcher Stats (threshold " + std::to_string(jit_threshold) + ")");
		dispatcher.PrintStats();
	}

	PrintBanner("Comparison across thresholds");
	std::cout << "\n"
	          << std::left << std::setw(14) << "Threshold" << std::right << std::setw(16) << "Total Vec (ms)" << std::setw(16)
	          << "Total JIT (ms)" << std::setw(14) << "Speedup"
	          << "\n";
	std::cout << std::string(60, '-') << "\n";
	for (const auto &s : all_summaries) {
		std::cout << std::left << std::setw(14) << s.threshold << std::right << std::fixed << std::setprecision(1)
		          << std::setw(16) << s.total_vec << std::setw(16) << s.total_jit << std::setprecision(2) << std::setw(14)
		          << s.overall_speedup << "x\n";
	}
	std::cout << "\n";

	{
		std::ofstream f(csv_out);
		if (!f.is_open()) {
			std::cerr << "WARNING: could not write " << csv_out << "\n";
		} else {
			f << "threshold,query,vectorized_ms,jit_ms,speedup,jit_error\n";
			for (const auto &pair : all_rows) {
				idx_t th = pair.first;
				for (const auto &r : pair.second) {
					f << th << "," << r.query_name << "," << r.vec_ms << "," << (r.jit_error ? -1.0 : r.jit_ms) << ","
					  << r.speedup << "," << (r.jit_error ? "true" : "false") << "\n";
				}
			}
			std::cout << "Results written to: " << csv_out << " (includes threshold column)\n";
		}
	}

	return 0;
}
