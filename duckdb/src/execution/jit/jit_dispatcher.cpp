//===----------------------------------------------------------------------===//
// CSCI 543 — JITDispatcher: choose compiled code vs interpreter.
//
// Flow: TryExecuteJIT → [JIT off?] fall through → cache Lookup → on miss, if hot enough,
// TryCompile → Lookup again → run cached std::function or count interpreter path.
//
// Intended call site: ExpressionExecutor::Execute after JITProfiler::Record (same batch
// `count` and column pointers into the current chunk).
// a full integration calls JITCompiler::Compile, wraps JITCompiledFn for JITCache::Insert.
//===----------------------------------------------------------------------===//
#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/execution/jit/jit_cache.hpp"
#include "duckdb/execution/jit/jit_compiler.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/types/vector.hpp"

#include <iostream>
#include <mutex>

namespace duckdb {

JITDispatcher::JITDispatcher()
    : compilation_threshold(1000), enable_jit(false), jit_executions(0), interpreter_executions(0),
      compilation_attempts(0), compilation_successes(0), compilation_failures(0) {
	JITProfiler::GetInstance().SetHotnessThreshold(compilation_threshold);
}

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
	// Keep profiler "hot" labeling in sync so IsExpressionHot / exports match compile policy.
	JITProfiler::GetInstance().SetHotnessThreshold(threshold);
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
	// Serialise compilation — JITCompiler::Compile is not thread-safe.
	// Check the cache first under the lock so we never double-compile.
	static std::mutex compile_mutex;
	std::lock_guard<std::mutex> compile_guard(compile_mutex);

	if (JITCache::GetInstance().Lookup(expr)) {
		// Another thread compiled this while we were waiting for the lock.
		return true;
	}

	compilation_attempts++;

	// Ask the LLVM compiler if it can handle this expression tree.
	auto &compiler = JITCompiler::GetInstance();
	if (!compiler.CanCompile(expr)) {
		compilation_failures++;
		return false;
	}

	// Compile to a raw native function pointer.
	JITCompiledFn fn = compiler.Compile(expr);
	if (!fn) {
		compilation_failures++;
		return false;
	}

	std::cout << "[JIT] Compiled expression fingerprint " << JITProfiler::Fingerprint(expr) << std::endl;

	// ── Build a Vector-level wrapper around the raw JIT function ──────────────
	//
	// Raw fn signature:
	//   void fn(void** col_data, uint8_t** col_valid,
	//           void* result_data, uint8_t* result_valid, uint64_t count)
	//
	// col_data[c] / col_valid[c] are indexed by BoundRef::index (source column
	// number in the DataChunk).  The executor passes col_ptrs[c] = &chunk->data[c]
	// so col_data[c] maps to the correct raw column array.
	//
	// Safety: we never Flatten() the caller's columns in-place; instead we create
	// local Reference copies and flatten only those, so the shared DataChunk
	// buffers are never modified.
	// ─────────────────────────────────────────────────────────────────────────
	const LogicalType return_type = expr.return_type;

	JITCompiledFunction wrapper = [fn, return_type](Vector **inputs, Vector &result, idx_t count) {
		constexpr idx_t MAX_COLS = 64;

		// Local flat copies of each input column (preserves originals).
		vector<Vector> flat_copies;
		flat_copies.reserve(MAX_COLS);
		vector<vector<uint8_t>> valid_bufs; // per-byte validity arrays for JIT fn
		valid_bufs.reserve(MAX_COLS);

		void    *col_data [MAX_COLS] = {};
		uint8_t *col_valid[MAX_COLS] = {};

		for (idx_t c = 0; c < MAX_COLS && inputs[c]; c++) {
			// Shallow reference then flatten only the local copy
			flat_copies.emplace_back(inputs[c]->GetType());
			flat_copies.back().Reference(*inputs[c]);
			flat_copies.back().Flatten(count);

			col_data[c] = FlatVector::GetData(flat_copies.back());

			// Convert DuckDB bitmask validity → per-byte (1=valid, 0=null)
			auto &validity = FlatVector::Validity(flat_copies.back());
			valid_bufs.emplace_back(count);
			for (idx_t r = 0; r < count; r++) {
				valid_bufs.back()[r] = validity.RowIsValid(r) ? 1 : 0;
			}
			col_valid[c] = valid_bufs.back().data();
		}

		// Allocate a fresh flat output vector and get its raw data pointer.
		// 'out_vec' is FLAT_VECTOR by default; Flatten() is therefore a no-op.
		Vector out_vec(return_type);
		out_vec.Flatten(count);
		void *result_data = FlatVector::GetData(out_vec);

		// Output validity byte array (pre-filled as all valid).
		vector<uint8_t> result_valid_buf(count, 1);

		// Run the JIT-compiled native function.
		fn(col_data, col_valid, result_data, result_valid_buf.data(), static_cast<uint64_t>(count));

		// Propagate NULL markers back into the output vector's validity mask.
		auto &out_validity = FlatVector::Validity(out_vec);
		for (idx_t r = 0; r < count; r++) {
			if (result_valid_buf[r] == 0) {
				out_validity.SetInvalid(r);
			}
		}

		// Point the caller's result vector at our output (shared buffer reference).
		result.Reference(out_vec);
	};

	JITCache::GetInstance().Insert(expr, std::move(wrapper), /*compile_time_us=*/0);
	compilation_successes++;
	return true;
}

} // namespace duckdb
