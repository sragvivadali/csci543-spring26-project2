//===----------------------------------------------------------------------===//
// CSCI 543 — JITDispatcher: choose compiled code vs interpreter.
//
// Flow: TryExecuteJIT → [JIT off?] fall through → cache Lookup → on miss, if hot enough,
// TryCompile → Lookup again → run cached std::function or count interpreter path.
//
// Intended call site: ExpressionExecutor::Execute after JITProfiler::Record (same batch
// `count` and column pointers into the current chunk). TryCompile is still a stub here;
// a full integration calls JITCompiler::Compile, wraps JITCompiledFn for JITCache::Insert.
//===----------------------------------------------------------------------===//
#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/execution/jit/jit_cache.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/vector.hpp"

#include <iostream>

namespace duckdb {

JITDispatcher &JITDispatcher::GetInstance() {
	static JITDispatcher instance;
	return instance;
}

bool JITDispatcher::TryExecuteJIT(const Expression &expr, ExpressionExecutor &executor, Vector **inputs, Vector &result,
                                  idx_t count) {
	(void)executor; // Reserved for future context (allocator, settings) when running JIT.
	if (!enable_jit) {
		interpreter_executions++;
		return false;
	}

	// 1) Happy path: fingerprint hit → run Vector-level wrapper (may bridge to native code).
	auto cached_func = JITCache::GetInstance().Lookup(expr);
	if (cached_func) {
		cached_func(inputs, result, count);
		jit_executions++;
		return true;
	}

	// 2) Cold cache: compile only if profiler says this tree crossed compilation_threshold.
	if (ShouldCompile(expr)) {
		if (TryCompile(expr)) {
			cached_func = JITCache::GetInstance().Lookup(expr);
			if (cached_func) {
				cached_func(inputs, result, count);
				jit_executions++;
				return true;
			}
		}
	}

	interpreter_executions++;
	return false;
}

void JITDispatcher::SetCompilationThreshold(idx_t threshold) {
	compilation_threshold = threshold;
}

void JITDispatcher::SetEnableJIT(bool enable) {
	enable_jit = enable;
}

JITDispatcher::DispatchStats JITDispatcher::GetStats() const {
	return {jit_executions, interpreter_executions, compilation_attempts, compilation_successes, compilation_failures};
}

void JITDispatcher::ResetStats() {
	jit_executions = 0;
	interpreter_executions = 0;
	compilation_attempts = 0;
	compilation_successes = 0;
	compilation_failures = 0;
}

void JITDispatcher::PrintStats() const {
	std::cout << "=== JIT Dispatcher Statistics ===" << std::endl;
	std::cout << "JIT enabled: " << (enable_jit ? "YES" : "NO") << std::endl;
	std::cout << "Compilation threshold: " << compilation_threshold << " tuples" << std::endl;
	std::cout << "\nExecution counts:" << std::endl;
	std::cout << "  JIT executions: " << jit_executions << std::endl;
	std::cout << "  Interpreter executions: " << interpreter_executions << std::endl;

	idx_t total = jit_executions + interpreter_executions;
	if (total > 0) {
		double jit_pct = (100.0 * jit_executions) / total;
		std::cout << "  JIT usage: " << jit_pct << "%" << std::endl;
	}

	std::cout << "\nCompilation:" << std::endl;
	std::cout << "  Attempts: " << compilation_attempts << std::endl;
	std::cout << "  Successes: " << compilation_successes << std::endl;
	std::cout << "  Failures: " << compilation_failures << std::endl;

	if (compilation_attempts > 0) {
		double success_rate = (100.0 * compilation_successes) / compilation_attempts;
		std::cout << "  Success rate: " << success_rate << "%" << std::endl;
	}
}

bool JITDispatcher::ShouldCompile(const Expression &expr) const {
	// Hotness = sum of tuple_counts passed to JITProfiler::Record for this fingerprint.
	uint64_t count = JITProfiler::GetInstance().GetCount(expr);
	return count >= compilation_threshold;
}

bool JITDispatcher::TryCompile(const Expression &expr) {
	compilation_attempts++;

	// STUB: replace with JITCompiler::GetInstance().Compile(expr), wrap JITCompiledFn into
	// JITCompiledFunction, JITCache::GetInstance().Insert(expr, wrapper, compile_time_us).
	// Until then, stats reflect attempts/failures and the interpreter always runs.
	std::cout << "[JIT] Compilation attempted for expression fingerprint " << JITProfiler::Fingerprint(expr)
	          << " (stub - waiting for LLVM compiler)" << std::endl;

	compilation_failures++;
	return false;
}

} // namespace duckdb
