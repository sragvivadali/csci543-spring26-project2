#include "duckdb.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"

using namespace duckdb;

int main() {
	// Clear previous process-local stats so the demo prints only this run.
	JITProfiler::GetInstance().Reset();

	DuckDB db(nullptr);
	Connection con(db);

	// Build a simple synthetic table with 1M rows and two DOUBLE columns.
	con.Query("CREATE TABLE t AS SELECT random()::DOUBLE * 100.0 AS a, random()::DOUBLE * 100.0 AS b FROM range(1000000)");
	auto result = con.Query("SELECT a * b FROM t WHERE a * 1.1 > 50");
	if (result->HasError()) {
		result->Print();
		return 1;
	}
	// Prints lines like:
	// "Expression fingerprint <u64> evaluated <count> times"
	JITProfiler::GetInstance().PrintStats();
	return 0;
}
