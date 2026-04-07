#include "duckdb.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"

using namespace duckdb;

int main() {
	JITProfiler::GetInstance().Reset();

	DuckDB db(nullptr);
	Connection con(db);

	// Build a simple synthetic table with 1M rows and two DOUBLE columns.
	con.Query("CREATE TABLE t AS SELECT random()::DOUBLE * 100.0 AS a, random()::DOUBLE * 100.0 AS b FROM range(1000000)");

	// Measure only the SELECT below: table creation also executes expressions.
	JITProfiler::GetInstance().Reset();

	// Hotness cutoff for this workload: main vectorized paths accumulate on the order of
	// 10^5–10^6 tuples; one-off batches stay in the single digits. 100k is a practical
	// JIT-oriented line between "noise" and "worth specializing" for the demo.
	constexpr uint64_t kHotnessThresholdTuples = 100000;
	JITProfiler::GetInstance().SetHotnessThreshold(kHotnessThresholdTuples);

	JITProfiler::PushQueryScope(1);
	auto result = con.Query("SELECT a * b FROM t WHERE a * 1.1 > 50");
	JITProfiler::PopQueryScope();
	if (result->HasError()) {
		result->Print();
		return 1;
	}
	// Prints global totals, optional per-query_id section, and overhead counters.
	JITProfiler::GetInstance().PrintStats();
	return 0;
}
