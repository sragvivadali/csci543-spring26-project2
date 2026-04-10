//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/jit/jit_benchmark.hpp
//
//===----------------------------------------------------------------------===//
//
// JITBenchmark — performance measurement and validation harness
//
// Purpose
// -------
// Provides utilities to:
// - Benchmark query execution with/without JIT
// - Validate that JIT produces same results as interpreter
// - Measure compilation overhead
// - Generate performance reports
//
// Usage
// -----
// JITBenchmark bench;
// bench.RunQuery("SELECT a + b FROM test");
// bench.PrintReport();
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/main/connection.hpp"

#include <string>
#include <vector>
#include <chrono>

namespace duckdb {

class DuckDB;

struct BenchmarkResult {
	string query;
	double interpreter_time_ms;
	double jit_time_ms;
	double speedup;
	bool results_match;
	idx_t row_count;
	idx_t compilation_count;
	
	BenchmarkResult() 
	    : interpreter_time_ms(0), jit_time_ms(0), speedup(0),
	      results_match(true), row_count(0), compilation_count(0) {
	}
};

class JITBenchmark {
public:
	JITBenchmark();
	explicit JITBenchmark(DuckDB &db);

	//! Run a single query and measure performance
	BenchmarkResult RunQuery(const string &query, idx_t iterations = 1);

	//! Run multiple queries and aggregate results
	void RunBenchmarkSuite(const vector<string> &queries, idx_t iterations = 3);

	//! Print detailed performance report
	void PrintReport() const;

	//! Export results to CSV
	void ExportCSV(const string &filename) const;

	//! Clear accumulated results
	void Clear();

	//! Get all benchmark results
	const vector<BenchmarkResult> &GetResults() const { return results; }

private:
	unique_ptr<DuckDB> owned_db;
	DuckDB *db;
	vector<BenchmarkResult> results;

	double MeasureQueryTime(Connection &conn, const string &query, bool enable_jit);
	bool ValidateResults(Connection &conn1, Connection &conn2, const string &query);
	string ResultsToString(unique_ptr<MaterializedQueryResult> result);
};

//! Utility class for high-precision timing
class Timer {
public:
	Timer() : start_time(std::chrono::high_resolution_clock::now()) {
	}

	void Reset() {
		start_time = std::chrono::high_resolution_clock::now();
	}

	double ElapsedMilliseconds() const {
		auto end_time = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
		return duration.count() / 1000.0;
	}

	uint64_t ElapsedMicroseconds() const {
		auto end_time = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
		return duration.count();
	}

private:
	std::chrono::high_resolution_clock::time_point start_time;
};

} // namespace duckdb
