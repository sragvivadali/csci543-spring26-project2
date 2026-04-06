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
// - Execution is vectorized: one "evaluation" in the executor processes up to N rows
//   (a batch). `Record(expr, tuple_count)` adds `tuple_count` to that expression's bucket
//   (typically the batch size, or the number of rows selected in a filter step).
// - Counts are keyed by a 64-bit *structural fingerprint* of the expression tree, not by
//   raw SQL text or pointer identity.
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
// - All public mutating/query methods take a mutex around the fingerprint → count map.
// - `Fingerprint()` is read-only and lock-free; it must stay pure for use from `Record`
//   and `GetCount` without deadlock.
//
// Instrumentation (where Record is called)
// ----------------------------------------
// - `ExpressionExecutor::Execute(const Expression &, ...)` after the `count == 0` guard.
// - `Select(BoundComparisonExpression)` and `Select(BoundBetweenExpression)` so predicates
//   evaluated on the selection path (e.g. filters) are counted even when the generic
//   `Execute` entry is not used for the root boolean.
//
// Typical usage
// -------------
//   JITProfiler::GetInstance().Reset();
//   // run query ...
//   JITProfiler::GetInstance().PrintStats();
//   // or query totals: GetCount(expr) after matching expr to the plan
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/planner/expression.hpp"

#include <mutex>

namespace duckdb {

//! Counts how often each expression (by structural fingerprint) is evaluated.
//! Thread-safe; intended to be called from expression execution paths.
class JITProfiler {
public:
	static JITProfiler &GetInstance();

	//! Increments the execution count for the fingerprint of `expr` by `tuple_count`
	//! (typically the number of rows in the current vector batch).
	void Record(const Expression &expr, idx_t tuple_count = 1);

	//! Structural fingerprint (FNV-1a over expression types, return types, and tree shape).
	//! Used for tests and tooling; must match what `Record` aggregates on.
	DUCKDB_API static uint64_t Fingerprint(const Expression &expr);

	//! Sum of tuple counts recorded for this expression's fingerprint.
	DUCKDB_API uint64_t GetCount(const Expression &expr) const;

	//! Dumps fingerprint → total tuple counts to stdout (for ad-hoc debugging).
	void PrintStats() const;

	//! Clears all accumulated counts (e.g. between queries or tests).
	void Reset();

private:
	JITProfiler() = default;

	mutable std::mutex lock;
	unordered_map<uint64_t, uint64_t> counts;
};

} // namespace duckdb
