#include "duckdb.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"

#include <iostream>

using namespace duckdb;

static void PrintEverythingRecorded(JITProfiler &prof) {
	std::cout
	    << "\n--- What the profiler actually stores ---\n"
	    << "  * Global map: fingerprint -> cumulative tuple_count (arithmetic roots only).\n"
	    << "  * Per-query map: same, when PushQueryScope was active on the executing thread.\n"
	    << "  * Overhead atomics: every Record() call for an arithmetic root bumps record_calls\n"
	    << "    and tuples_accounted; lock hold time is accumulated in locked_section_ns.\n"
	    << "  * Expressions that are NOT arithmetic roots at the Record() site (comparisons,\n"
	    << "    casts, generic functions, etc.) do not create or update map entries.\n"
	    << "  * Fingerprints are structural hashes (FNV-1a); they are not SQL text.\n\n";

	const auto snap = prof.ExportSnapshot();
	std::cout << "--- ExportSnapshot (full structured view) ---\n";
	std::cout << "hotness_threshold_configured: " << (snap.hotness_threshold_configured ? "true" : "false")
	          << "\n";
	std::cout << "hotness_threshold_value: " << snap.hotness_threshold_value << "\n";
	std::cout << "overhead.record_calls: " << snap.overhead.record_calls << "\n";
	std::cout << "overhead.tuples_accounted: " << snap.overhead.tuples_accounted << "\n";
	std::cout << "overhead.locked_section_nanoseconds: " << snap.overhead.locked_section_nanoseconds << "\n";
	std::cout << "global_by_fingerprint (" << snap.global_by_fingerprint.size() << " entries, sorted by fp):\n";
	for (const auto &e : snap.global_by_fingerprint) {
		std::cout << "  fp " << e.fingerprint << " -> tuples " << e.tuple_count << "\n";
	}
	std::cout << "by_query (" << snap.by_query.size() << " queries):\n";
	for (const auto &q : snap.by_query) {
		std::cout << "  query_id " << q.query_id << " (" << q.per_expression.size() << " fps):\n";
		for (const auto &e : q.per_expression) {
			std::cout << "    fp " << e.fingerprint << " -> tuples " << e.tuple_count << "\n";
		}
	}
	std::cout << "hot_labeled_fingerprints (" << snap.hot_labeled_fingerprints.size() << "):\n";
	for (const uint64_t fp : snap.hot_labeled_fingerprints) {
		std::cout << "  fp " << fp << "\n";
	}

	const auto top = prof.GetTopHotExpressions(NumericLimits<idx_t>::Maximum());
	std::cout << "\n--- Ranked by tuple count (hottest first; GetTopHotExpressions) ---\n";
	idx_t rank = 1;
	for (const auto &e : top) {
		std::cout << "  #" << rank++ << "  fp " << e.fingerprint << " -> tuples " << e.tuple_count << "\n";
	}
	std::cout << "\n";
}

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
	auto &prof = JITProfiler::GetInstance();
	prof.PrintStats();
	PrintEverythingRecorded(prof);
	return 0;
}
