#include "duckdb/execution/jit/jit_benchmark.hpp"
#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/execution/jit/jit_cache.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace duckdb {

JITBenchmark::JITBenchmark() {
	owned_db = make_uniq<DuckDB>(nullptr);
	db = owned_db.get();
}

JITBenchmark::JITBenchmark(DuckDB &db_ref) : db(&db_ref) {
}

BenchmarkResult JITBenchmark::RunQuery(const string &query, idx_t iterations) {
	BenchmarkResult result;
	result.query = query;

	Connection conn(*db);

	// Measure interpreter time (JIT disabled)
	JITDispatcher::GetInstance().SetEnableJIT(false);
	JITProfiler::GetInstance().Reset();
	JITCache::GetInstance().Clear();
	
	double total_interpreter_time = 0;
	for (idx_t i = 0; i < iterations; i++) {
		total_interpreter_time += MeasureQueryTime(conn, query, false);
	}
	result.interpreter_time_ms = total_interpreter_time / iterations;

	// Measure JIT time (JIT enabled)
	JITDispatcher::GetInstance().SetEnableJIT(true);
	JITDispatcher::GetInstance().SetCompilationThreshold(1000); // Low threshold for testing
	JITProfiler::GetInstance().Reset();
	JITCache::GetInstance().Clear();
	
	double total_jit_time = 0;
	for (idx_t i = 0; i < iterations; i++) {
		total_jit_time += MeasureQueryTime(conn, query, true);
	}
	result.jit_time_ms = total_jit_time / iterations;

	// Calculate speedup
	if (result.interpreter_time_ms > 0) {
		result.speedup = result.interpreter_time_ms / result.jit_time_ms;
	}

	// Validate results match
	Connection conn1(*db);
	Connection conn2(*db);
	result.results_match = ValidateResults(conn1, conn2, query);

	// Get row count
	auto query_result = conn.Query(query);
	if (query_result->HasError()) {
		std::cerr << "Query error: " << query_result->GetError() << std::endl;
	} else {
		auto materialized = unique_ptr_cast<QueryResult, MaterializedQueryResult>(std::move(query_result));
		result.row_count = materialized->RowCount();
	}

	// Get compilation stats
	auto dispatch_stats = JITDispatcher::GetInstance().GetStats();
	result.compilation_count = dispatch_stats.compilation_attempts;

	return result;
}

void JITBenchmark::RunBenchmarkSuite(const vector<string> &queries, idx_t iterations) {
	std::cout << "=== Running JIT Benchmark Suite ===" << std::endl;
	std::cout << "Queries: " << queries.size() << std::endl;
	std::cout << "Iterations per query: " << iterations << std::endl;
	std::cout << std::endl;

	for (const auto &query : queries) {
		std::cout << "Benchmarking: " << query.substr(0, 60) 
		          << (query.length() > 60 ? "..." : "") << std::endl;
		
		auto result = RunQuery(query, iterations);
		results.push_back(result);
		
		std::cout << "  Interpreter: " << std::fixed << std::setprecision(2) 
		          << result.interpreter_time_ms << "ms" << std::endl;
		std::cout << "  JIT: " << result.jit_time_ms << "ms" << std::endl;
		std::cout << "  Speedup: " << std::setprecision(2) << result.speedup << "x" << std::endl;
		std::cout << "  Results match: " << (result.results_match ? "YES" : "NO") << std::endl;
		std::cout << std::endl;
	}
}

void JITBenchmark::PrintReport() const {
	std::cout << "\n=== JIT Benchmark Report ===" << std::endl;
	std::cout << std::string(80, '=') << std::endl;
	
	if (results.empty()) {
		std::cout << "No benchmark results available." << std::endl;
		return;
	}

	// Summary statistics
	double total_interpreter_time = 0;
	double total_jit_time = 0;
	idx_t total_rows = 0;
	idx_t mismatches = 0;

	for (const auto &result : results) {
		total_interpreter_time += result.interpreter_time_ms;
		total_jit_time += result.jit_time_ms;
		total_rows += result.row_count;
		if (!result.results_match) {
			mismatches++;
		}
	}

	std::cout << "\nSummary:" << std::endl;
	std::cout << "  Total queries: " << results.size() << std::endl;
	std::cout << "  Total rows processed: " << total_rows << std::endl;
	std::cout << "  Result mismatches: " << mismatches << std::endl;
	std::cout << std::endl;

	std::cout << "  Total interpreter time: " << std::fixed << std::setprecision(2) 
	          << total_interpreter_time << "ms" << std::endl;
	std::cout << "  Total JIT time: " << total_jit_time << "ms" << std::endl;
	
	if (total_jit_time > 0) {
		double overall_speedup = total_interpreter_time / total_jit_time;
		std::cout << "  Overall speedup: " << std::setprecision(2) << overall_speedup << "x" << std::endl;
	}

	// Detailed results
	std::cout << "\n" << std::string(80, '-') << std::endl;
	std::cout << std::left << std::setw(40) << "Query" 
	          << std::right << std::setw(12) << "Interp(ms)" 
	          << std::setw(12) << "JIT(ms)" 
	          << std::setw(10) << "Speedup" 
	          << std::setw(6) << "Match" << std::endl;
	std::cout << std::string(80, '-') << std::endl;

	for (const auto &result : results) {
		string short_query = result.query.substr(0, 37);
		if (result.query.length() > 37) {
			short_query += "...";
		}

		std::cout << std::left << std::setw(40) << short_query
		          << std::right << std::fixed << std::setprecision(2)
		          << std::setw(12) << result.interpreter_time_ms
		          << std::setw(12) << result.jit_time_ms
		          << std::setw(10) << result.speedup
		          << std::setw(6) << (result.results_match ? "YES" : "NO")
		          << std::endl;
	}
	std::cout << std::string(80, '=') << std::endl;
}

void JITBenchmark::ExportCSV(const string &filename) const {
	std::ofstream file(filename);
	if (!file.is_open()) {
		std::cerr << "Failed to open file: " << filename << std::endl;
		return;
	}

	// CSV header
	file << "Query,Interpreter_ms,JIT_ms,Speedup,Results_Match,Row_Count,Compilation_Count\n";

	// Data rows
	for (const auto &result : results) {
		// Escape quotes in query
		string escaped_query = result.query;
		size_t pos = 0;
		while ((pos = escaped_query.find('"', pos)) != string::npos) {
			escaped_query.replace(pos, 1, "\"\"");
			pos += 2;
		}

		file << "\"" << escaped_query << "\","
		     << result.interpreter_time_ms << ","
		     << result.jit_time_ms << ","
		     << result.speedup << ","
		     << (result.results_match ? "true" : "false") << ","
		     << result.row_count << ","
		     << result.compilation_count << "\n";
	}

	file.close();
	std::cout << "Results exported to: " << filename << std::endl;
}

void JITBenchmark::Clear() {
	results.clear();
}

double JITBenchmark::MeasureQueryTime(Connection &conn, const string &query, bool enable_jit) {
	JITDispatcher::GetInstance().SetEnableJIT(enable_jit);
	
	Timer timer;
	auto result = conn.Query(query);
	
	if (result->HasError()) {
		std::cerr << "Query error: " << result->GetError() << std::endl;
		return 0;
	}

	// Materialize all results to ensure full execution
	if (result->type == QueryResultType::STREAM_RESULT) {
		auto materialized = unique_ptr_cast<QueryResult, MaterializedQueryResult>(std::move(result));
	}

	return timer.ElapsedMilliseconds();
}

bool JITBenchmark::ValidateResults(Connection &conn1, Connection &conn2, const string &query) {
	// Run with interpreter
	JITDispatcher::GetInstance().SetEnableJIT(false);
	auto result1 = conn1.Query(query);
	if (result1->HasError()) {
		return false;
	}

	// Run with JIT
	JITDispatcher::GetInstance().SetEnableJIT(true);
	auto result2 = conn2.Query(query);
	if (result2->HasError()) {
		return false;
	}

	// Compare results
	string str1 = ResultsToString(unique_ptr_cast<QueryResult, MaterializedQueryResult>(std::move(result1)));
	string str2 = ResultsToString(unique_ptr_cast<QueryResult, MaterializedQueryResult>(std::move(result2)));

	return str1 == str2;
}

string JITBenchmark::ResultsToString(unique_ptr<MaterializedQueryResult> result) {
	if (!result || result->HasError()) {
		return "";
	}

	std::stringstream ss;
	
	// Write column names
	for (idx_t col = 0; col < result->ColumnCount(); col++) {
		if (col > 0) ss << ",";
		ss << result->ColumnName(col);
	}
	ss << "\n";

	// Write data rows
	for (idx_t row = 0; row < result->RowCount(); row++) {
		for (idx_t col = 0; col < result->ColumnCount(); col++) {
			if (col > 0) ss << ",";
			auto value = result->GetValue(col, row);
			ss << value.ToString();
		}
		ss << "\n";
	}

	return ss.str();
}

} // namespace duckdb
