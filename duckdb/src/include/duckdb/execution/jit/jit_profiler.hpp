//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/jit/jit_profiler.hpp
//
//===----------------------------------------------------------------------===//
//
// JITProfiler — lightweight expression evaluation profiler (research / JIT groundwork)
//
// Purpose
// -------
// Counts how often each *bound* expression is executed during query evaluation. Intended
// as a hook point for experiments (e.g. identifying hot expression trees before JIT).
//
// What gets counted
// -----------------
// - **JIT-relevant roots:** `Record` updates stats when the root is either (a) a
//   `BoundFunctionExpression` for scalar arithmetic (`+`, `-`, `*`, `/`, `//`, `%`), or
//   (b) any expression `JITCompiler::CanCompile` accepts (numeric comparisons, conjunctions,
//   casts, etc.). Everything else is skipped.
// - Execution is vectorized: one "evaluation" in the executor processes up to N rows
//   (a batch). For eligible roots, `Record(expr, tuple_count)` adds `tuple_count` to that
//   expression's bucket (typically the batch size, or the selection path row count).
// - Counts are keyed by a 64-bit *structural fingerprint* of the expression tree, not by
//   raw SQL text or pointer identity.
//
// Per-query grouping
// ------------------
// - Global totals are always updated. Optional per-query buckets are updated when this
//   thread has an active query scope (see `PushQueryScope` / `PopQueryScope`).
// - Executor hooks only call `Record`; tests or embedding code can bracket work with
//   query scopes to attribute counts to a logical `query_id`.
//
// Fingerprint (structural hash)
// -----------------------------
// - Algorithm: FNV-1a over a preorder-style walk of the bound expression tree.
// - Per node we mix: ExpressionType, ExpressionClass, LogicalType id + PhysicalType, and
//   child structure. For functions we also mix the bound function name.
// - `BoundConstantExpression` values are *not* hashed: two literals of the same logical
//   type share a fingerprint (see tests). Column refs include binding/index so different
//   slots differ.
// - Collisions are possible in principle (64-bit hash); treat fingerprints as a heuristic.
//
// Thread safety
// -------------
// - All public mutating/query methods on shared state take a mutex around internal maps.
// - Overhead counters (`record_calls`, tuple sums, locked-section timing) use atomics.
// - `Fingerprint()` is read-only and lock-free; it must stay pure for use from `Record`
//   and `GetCount` without deadlock.
// - Query scopes are thread-local; each thread maintains its own stack of active query ids.
//
// Instrumentation (where Record is called)
// ----------------------------------------
// - `ExpressionExecutor::Execute(const Expression &, ...)` after the `count == 0` guard.
// - `Select(BoundComparisonExpression)` / `Select(BoundBetweenExpression)` call `Record` for
//   filter batches; compilable predicates increment hotness alongside projection paths.
//
// Typical usage
// -------------
//   JITProfiler::GetInstance().Reset();
//   JITProfiler::PushQueryScope(1);
//   // run query ...
//   JITProfiler::PopQueryScope();
//   JITProfiler::GetInstance().PrintStats();
//   // or ExportSnapshot() for structured inspection
//
// JIT compilation hookup
// ----------------------
// - `JITDispatcher::SetCompilationThreshold` also calls `SetHotnessThreshold` with the same
//   value so hot labels match compile policy. The dispatcher default is 1000 tuples.
// - Use `IsExpressionHot(expr)` or `IsFingerprintHot(fp)` before enqueueing JIT work.
// - Optionally `SetFingerprintBecameHotCallback` to push a fingerprint into a compile queue
//   the first time it crosses the threshold (runs outside the profiler lock).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/planner/expression.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>

namespace duckdb {

//! Single fingerprint bucket (stable sort key: fingerprint ascending in exports).
struct JITProfilerFingerprintCount {
	uint64_t fingerprint;
	uint64_t tuple_count;
};

//! Per-query fingerprint buckets (expressions sorted by fingerprint in exports).
struct JITProfilerQueryFingerprintStats {
	idx_t query_id;
	vector<JITProfilerFingerprintCount> per_expression;
};

//! Bookkeeping for profiler-induced work (lock hold + map updates on the Record path).
struct JITProfilerOverheadStats {
	//! Number of `Record` calls observed.
	uint64_t record_calls;
	//! Sum of `tuple_count` arguments passed to `Record`.
	uint64_t tuples_accounted;
	//! Cumulative time spent inside the critical section (lock + map updates), in nanoseconds.
	uint64_t locked_section_nanoseconds;
};

//! Stable, structured snapshot for tests and tools (sorted keys throughout).
struct JITProfilerExportData {
	vector<JITProfilerFingerprintCount> global_by_fingerprint;
	vector<JITProfilerQueryFingerprintStats> by_query;
	bool hotness_threshold_configured = false;
	uint64_t hotness_threshold_value = 0;
	//! Fingerprints currently labeled hot (see `IsFingerprintHot`); sorted ascending.
	//! Empty when no threshold is configured (JIT should use `IsFingerprintHot` / counts instead).
	vector<uint64_t> hot_labeled_fingerprints;
	JITProfilerOverheadStats overhead {};
};

//! Counts how often each expression (by structural fingerprint) is evaluated.
//! Thread-safe; intended to be called from expression execution paths.
class JITProfiler {
public:
	static JITProfiler &GetInstance();

	//! Thread-local: push `query_id` so subsequent `Record` calls on this thread also
	//! attribute tuple counts to this query. Nestable (stack).
	static void PushQueryScope(idx_t query_id);
	//! Pops the innermost query scope for this thread; no-op if the stack is empty.
	static void PopQueryScope();

	//! Increments the execution count for the fingerprint of `expr` by `tuple_count`
	//! (typically the number of rows in the current vector batch).
	void Record(const Expression &expr, idx_t tuple_count = 1);

	//! Structural fingerprint (FNV-1a over expression types, return types, and tree shape).
	//! Used for tests and tooling; must match what `Record` aggregates on.
	DUCKDB_API static uint64_t Fingerprint(const Expression &expr);

	//! True iff `expr` is a bound scalar arithmetic operator (`+`, `-`, `*`, `/`, `//`, `%`).
	//! `Record` also includes any expression `JITCompiler::CanCompile` accepts; use this
	//! helper when you only care about the arithmetic subset.
	DUCKDB_API static bool ProfilesAsArithmeticRoot(const Expression &expr);

	//! Sum of tuple counts recorded for this expression's fingerprint (global totals).
	DUCKDB_API uint64_t GetCount(const Expression &expr) const;

	//! Sum of tuple counts for this fingerprint within `query_id` (0 if none).
	DUCKDB_API uint64_t GetCountForQuery(idx_t query_id, const Expression &expr) const;

	//! Global fingerprint → tuple totals, sorted by fingerprint ascending.
	DUCKDB_API vector<JITProfilerFingerprintCount> GetGlobalExpressionStats() const;

	//! Per-query fingerprint totals; outer vector sorted by query_id, inner by fingerprint.
	DUCKDB_API vector<JITProfilerQueryFingerprintStats> GetPerQueryExpressionStats() const;

	//! Top `limit` fingerprints by global tuple count (descending); ties broken by lower fingerprint.
	DUCKDB_API vector<JITProfilerFingerprintCount> GetTopHotExpressions(idx_t limit) const;

	//! Minimum tuple count for an expression to count as "hot" when a threshold is configured.
	//! When not configured, any positive count passes.
	//! Setting the threshold rebuilds `hot_labeled_fingerprints` from current global counts (no callback).
	void SetHotnessThreshold(uint64_t minimum_tuple_count);
	void ClearHotnessThreshold();
	bool HasHotnessThreshold() const;
	std::optional<uint64_t> GetHotnessThreshold() const;
	bool PassesHotnessThreshold(uint64_t tuple_count) const;

	//! JIT integration: whether this fingerprint is labeled hot under current policy.
	//! - With threshold: true iff global tuple count for `fingerprint` is >= threshold (mirrors internal label set).
	//! - Without threshold: true iff the fingerprint has been recorded with a positive total count.
	DUCKDB_API bool IsFingerprintHot(uint64_t fingerprint) const;
	DUCKDB_API bool IsExpressionHot(const Expression &expr) const;

	//! Sorted list of fingerprints labeled hot when a threshold is configured; empty otherwise.
	DUCKDB_API vector<uint64_t> GetHotLabeledFingerprints() const;

	//! Optional hook: invoked at most once per fingerprint when it first crosses the hotness threshold
	//! during `Record` (not when labels are rebuilt via `SetHotnessThreshold`). Intended for a future
	//! JIT compilation queue; keep the callback non-blocking or offload work to avoid slowing execution.
	void SetFingerprintBecameHotCallback(std::function<void(uint64_t fingerprint)> callback);
	void ClearFingerprintBecameHotCallback();

	//! Point-in-time overhead counters (also included in `ExportSnapshot`).
	JITProfilerOverheadStats GetOverheadStats() const;

	//! Full snapshot with deterministic ordering for assertions.
	DUCKDB_API JITProfilerExportData ExportSnapshot() const;

	//! Human-readable dump to stdout (global, per-query, threshold, overhead summary).
	void PrintStats() const;

	//! Clears accumulated counts, per-query maps, overhead counters, hot labels, threshold, and callback.
	void Reset();

private:
	JITProfiler() = default;

	void RebuildHotLabelsLocked();

	mutable std::mutex lock;
	unordered_map<uint64_t, uint64_t> counts;
	unordered_map<idx_t, unordered_map<uint64_t, uint64_t>> per_query_counts;
	std::optional<uint64_t> hotness_threshold;
	//! Populated when a threshold is set; fingerprints whose global count meets the threshold.
	unordered_set<uint64_t> hot_labeled_fingerprints;
	std::function<void(uint64_t)> on_fingerprint_became_hot;

	std::atomic<uint64_t> record_invocations {0};
	std::atomic<uint64_t> tuples_accounted {0};
	std::atomic<uint64_t> locked_section_nanoseconds {0};
};

} // namespace duckdb
