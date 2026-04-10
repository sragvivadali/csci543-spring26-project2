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

bool JITDispatcher::TryExecuteJIT(const Expression &expr, ExpressionExecutor &executor,
                                   Vector **inputs, Vector &result, idx_t count) {
	if (!enable_jit) {
		interpreter_executions++;
		return false;
	}

	// Try cache lookup first
	auto cached_func = JITCache::GetInstance().Lookup(expr);
	if (cached_func) {
		// Execute compiled code
		cached_func(inputs, result, count);
		jit_executions++;
		return true;
	}

	// Check if we should compile this expression
	if (ShouldCompile(expr)) {
		if (TryCompile(expr)) {
			// Try lookup again after compilation
			cached_func = JITCache::GetInstance().Lookup(expr);
			if (cached_func) {
				cached_func(inputs, result, count);
				jit_executions++;
				return true;
			}
		}
	}

	// Fall back to interpreter
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
	return {jit_executions, interpreter_executions, compilation_attempts,
	        compilation_successes, compilation_failures};
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
	// Check if expression has been executed enough times
	uint64_t count = JITProfiler::GetInstance().GetCount(expr);
	return count >= compilation_threshold;
}

bool JITDispatcher::TryCompile(const Expression &expr) {
	compilation_attempts++;
	
	// TODO: This is a stub. Person B will implement actual LLVM compilation.
	// For now, we just track that compilation was attempted but not successful.
	// When Person B implements the compiler, this will:
	// 1. Generate LLVM IR for the expression
	// 2. Compile to native code
	// 3. Insert into cache
	// 4. Return true on success
	
	std::cout << "[JIT] Compilation attempted for expression fingerprint " 
	          << JITProfiler::Fingerprint(expr) 
	          << " (stub - waiting for LLVM compiler)" << std::endl;
	
	compilation_failures++;
	return false;
}

} // namespace duckdb
