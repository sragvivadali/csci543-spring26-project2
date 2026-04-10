//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/jit/jit_dispatcher.hpp
//
//===----------------------------------------------------------------------===//
//
// JITDispatcher — runtime dispatcher for JIT vs interpreter execution
//
// Purpose
// -------
// Decides whether to use compiled JIT code or fall back to interpreter.
// Uses profiling data and cache state to make intelligent decisions.
//
// Decision logic
// --------------
// 1. Check if expression has compiled code in cache
// 2. If not, check if expression is "hot" enough to compile
// 3. If hot and not cached, trigger compilation (stub for now)
// 4. Otherwise use interpreter
//
// Configuration
// -------------
// - compilation_threshold: min tuple count before compiling (default 10000)
// - enable_jit: master switch to enable/disable JIT (default false)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

class Vector;
class DataChunk;
class ExpressionExecutor;

class JITDispatcher {
public:
	static JITDispatcher &GetInstance();

	//! Main dispatch point: execute expression using JIT or interpreter
	//! Returns true if JIT was used, false if interpreter was used
	bool TryExecuteJIT(const Expression &expr, ExpressionExecutor &executor, 
	                   Vector **inputs, Vector &result, idx_t count);

	//! Configuration
	void SetCompilationThreshold(idx_t threshold);
	void SetEnableJIT(bool enable);
	
	idx_t GetCompilationThreshold() const { return compilation_threshold; }
	bool IsJITEnabled() const { return enable_jit; }

	//! Statistics
	struct DispatchStats {
		idx_t jit_executions;
		idx_t interpreter_executions;
		idx_t compilation_attempts;
		idx_t compilation_successes;
		idx_t compilation_failures;
	};
	DispatchStats GetStats() const;
	void ResetStats();

	//! Print dispatch statistics
	void PrintStats() const;

private:
	JITDispatcher() 
	    : compilation_threshold(10000), enable_jit(false),
	      jit_executions(0), interpreter_executions(0),
	      compilation_attempts(0), compilation_successes(0), 
	      compilation_failures(0) {
	}

	idx_t compilation_threshold;
	bool enable_jit;

	// Statistics (not thread-safe for simplicity, can add mutex if needed)
	idx_t jit_executions;
	idx_t interpreter_executions;
	idx_t compilation_attempts;
	idx_t compilation_successes;
	idx_t compilation_failures;

	bool ShouldCompile(const Expression &expr) const;
	bool TryCompile(const Expression &expr);
};

} // namespace duckdb
