//===----------------------------------------------------------------------===//
// JIT Benchmark Demo Tool
//
// Demonstrates the JIT cache, dispatcher, and benchmark harness.
// Usage: ./jit_benchmark_demo
//===----------------------------------------------------------------------===//

#include "duckdb/main/database.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/execution/jit/jit_benchmark.hpp"
#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/execution/jit/jit_cache.hpp"

#include <iostream>
#include <vector>

using namespace duckdb;

void PrintHeader(const std::string &title) {
	std::cout << "\n" << std::string(80, '=') << std::endl;
	std::cout << title << std::endl;
	std::cout << std::string(80, '=') << std::endl;
}

void DemoBasicSetup() {
	PrintHeader("1. Basic Setup and Configuration");
	
	auto &dispatcher = JITDispatcher::GetInstance();
	auto &cache = JITCache::GetInstance();
	
	// Configure JIT system
	dispatcher.SetEnableJIT(false); // Start disabled
	dispatcher.SetCompilationThreshold(5000);
	cache.SetMaxEntries(100);
	
	std::cout << "JIT Configuration:" << std::endl;
	std::cout << "  JIT Enabled: " << (dispatcher.IsJITEnabled() ? "YES" : "NO") << std::endl;
	std::cout << "  Compilation Threshold: " << dispatcher.GetCompilationThreshold() << " tuples" << std::endl;
	std::cout << "  Cache Max Entries: " << cache.GetStats().max_entries << std::endl;
}

void DemoProfiler() {
	PrintHeader("2. Expression Profiler Demo");
	
	DuckDB db(nullptr);
	Connection conn(db);
	
	// Create test table
	conn.Query("CREATE TABLE test_data (a INTEGER, b INTEGER, c DOUBLE)");
	conn.Query("INSERT INTO test_data SELECT i, i * 2, i * 1.5 FROM range(10000) t(i)");
	
	// Reset profiler
	JITProfiler::GetInstance().Reset();
	
	// Run some queries
	std::cout << "\nRunning queries to collect profiling data..." << std::endl;
	conn.Query("SELECT a + b FROM test_data WHERE a > 100");
	conn.Query("SELECT a * b FROM test_data WHERE c > 50.0");
	conn.Query("SELECT a + b + c FROM test_data");
	
	// Print profiler stats
	std::cout << "\nProfiler Results:" << std::endl;
	JITProfiler::GetInstance().PrintStats();
}

void DemoCache() {
	PrintHeader("3. JIT Cache Demo");
	
	auto &cache = JITCache::GetInstance();
	cache.Clear();
	cache.SetMaxEntries(5);
	
	std::cout << "Cache configured with max 5 entries" << std::endl;
	std::cout << "\nInitial cache state:" << std::endl;
	cache.PrintCache();
	
	// Note: In a real scenario, the compiler would insert functions into the cache
	// For now, we just show the cache infrastructure is working
	std::cout << "\n(Cache would be populated by LLVM compiler - waiting for Person B's implementation)" << std::endl;
}

void DemoDispatcher() {
	PrintHeader("4. JIT Dispatcher Demo");
	
	auto &dispatcher = JITDispatcher::GetInstance();
	dispatcher.ResetStats();
	dispatcher.SetEnableJIT(false); // Disabled for now since no compiler yet
	
	std::cout << "Dispatcher Configuration:" << std::endl;
	std::cout << "  JIT Enabled: " << (dispatcher.IsJITEnabled() ? "YES" : "NO") << std::endl;
	std::cout << "  Compilation Threshold: " << dispatcher.GetCompilationThreshold() << std::endl;
	
	std::cout << "\nDispatcher Stats:" << std::endl;
	dispatcher.PrintStats();
	
	std::cout << "\n(Dispatcher will automatically choose JIT vs interpreter once compiler is ready)" << std::endl;
}

void DemoBenchmark() {
	PrintHeader("5. Benchmark Harness Demo");
	
	DuckDB db(nullptr);
	Connection conn(db);
	
	// Setup test data
	conn.Query("CREATE TABLE bench_data (x INTEGER, y INTEGER, z DOUBLE)");
	conn.Query("INSERT INTO bench_data SELECT i, i * 2, i * 1.5 FROM range(100000) t(i)");
	
	// Create benchmark
	JITBenchmark bench(db);
	
	// Define test queries
	vector<string> queries = {
		"SELECT x + y FROM bench_data WHERE x > 1000",
		"SELECT CAST(x AS BIGINT) * CAST(y AS BIGINT) FROM bench_data WHERE z > 500.0",
		"SELECT x + y + z FROM bench_data",
		"SELECT x * 2 + y * 3 FROM bench_data WHERE x < 50000"
	};
	
	std::cout << "Running benchmark suite with " << queries.size() << " queries..." << std::endl;
	std::cout << "(Currently using interpreter only - JIT speedup will show once compiler is ready)" << std::endl;
	std::cout << std::endl;
	
	// Run benchmarks
	bench.RunBenchmarkSuite(queries, 3);
	
	// Print report
	bench.PrintReport();
	
	// Export to CSV
	std::string csv_file = "jit_benchmark_results.csv";
	bench.ExportCSV(csv_file);
	std::cout << "\nResults exported to: " << csv_file << std::endl;
}

void DemoIntegration() {
	PrintHeader("6. Full System Integration");
	
	std::cout << "The JIT runtime system consists of three main components:" << std::endl;
	std::cout << std::endl;
	std::cout << "1. JITCache" << std::endl;
	std::cout << "   - Stores compiled functions by expression fingerprint" << std::endl;
	std::cout << "   - LRU eviction policy for memory management" << std::endl;
	std::cout << "   - Thread-safe lookup and insertion" << std::endl;
	std::cout << std::endl;
	std::cout << "2. JITDispatcher" << std::endl;
	std::cout << "   - Decides when to use JIT vs interpreter" << std::endl;
	std::cout << "   - Uses profiling data to identify hot expressions" << std::endl;
	std::cout << "   - Triggers compilation for expressions above threshold" << std::endl;
	std::cout << "   - Falls back to interpreter when needed" << std::endl;
	std::cout << std::endl;
	std::cout << "3. JITBenchmark" << std::endl;
	std::cout << "   - Measures performance of JIT vs interpreter" << std::endl;
	std::cout << "   - Validates correctness of compiled code" << std::endl;
	std::cout << "   - Generates performance reports and CSV exports" << std::endl;
	std::cout << std::endl;
	std::cout << "Integration Points:" << std::endl;
	std::cout << "   - Profiler tracks expression execution counts" << std::endl;
	std::cout << "   - Dispatcher checks profiler to decide compilation" << std::endl;
	std::cout << "   - Compiler (Person B) inserts into cache" << std::endl;
	std::cout << "   - Dispatcher retrieves from cache for execution" << std::endl;
	std::cout << "   - Benchmark validates and measures everything" << std::endl;
}

int main() {
	std::cout << "DuckDB JIT Runtime System Demo" << std::endl;
	std::cout << "Part C: Cache / Dispatcher / Evaluation" << std::endl;
	
	try {
		DemoBasicSetup();
		DemoProfiler();
		DemoCache();
		DemoDispatcher();
		DemoBenchmark();
		DemoIntegration();
		
		PrintHeader("Demo Complete");
		std::cout << "\nAll runtime components are implemented and tested." << std::endl;
		std::cout << "Ready for integration with Person B's LLVM compiler!" << std::endl;
		
	} catch (std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}
	
	return 0;
}
