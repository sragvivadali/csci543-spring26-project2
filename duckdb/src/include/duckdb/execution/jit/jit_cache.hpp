//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/jit/jit_cache.hpp
//
//===----------------------------------------------------------------------===//
//
// JITCache — compiled function cache for JIT expressions
//
// Purpose
// -------
// Stores compiled native functions keyed by expression fingerprint. Provides
// lookup, insertion, and LRU eviction to manage memory.
//
// Thread safety
// -------------
// All public methods are thread-safe via internal mutex.
//
// Cache policy
// ------------
// - LRU eviction when cache exceeds max_entries
// - Fingerprint-based lookup (same as JITProfiler)
// - Each entry stores a function pointer and metadata
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/planner/expression.hpp"

#include <list>
#include <mutex>
#include <functional>

namespace duckdb {

class Vector;
class DataChunk;

// Function signature for compiled JIT code
// Takes input vectors, output vector, and count
using JITCompiledFunction = std::function<void(Vector **, Vector &, idx_t)>;

struct JITCacheEntry {
	JITCompiledFunction function;
	uint64_t fingerprint;
	idx_t hit_count;
	idx_t compile_time_us;
	
	JITCacheEntry(JITCompiledFunction func, uint64_t fp, idx_t compile_time)
	    : function(std::move(func)), fingerprint(fp), hit_count(0), compile_time_us(compile_time) {
	}
};

class JITCache {
public:
	static JITCache &GetInstance();

	//! Lookup compiled function by expression fingerprint
	//! Returns nullptr if not found
	JITCompiledFunction Lookup(const Expression &expr);

	//! Insert compiled function into cache
	//! If cache is full, evicts LRU entry
	void Insert(const Expression &expr, JITCompiledFunction function, idx_t compile_time_us);

	//! Clear all cached entries
	void Clear();

	//! Get cache statistics
	struct CacheStats {
		idx_t total_entries;
		idx_t total_hits;
		idx_t total_misses;
		idx_t total_evictions;
		idx_t max_entries;
	};
	CacheStats GetStats() const;

	//! Configure max cache size (default 1000)
	void SetMaxEntries(idx_t max);

	//! Print cache contents for debugging
	void PrintCache() const;

private:
	JITCache() : max_entries(1000), total_hits(0), total_misses(0), total_evictions(0) {
	}

	mutable std::mutex lock;
	idx_t max_entries;
	idx_t total_hits;
	idx_t total_misses;
	idx_t total_evictions;

	// LRU tracking: list of fingerprints (most recent at front)
	std::list<uint64_t> lru_list;
	
	// Cache storage: fingerprint -> (entry, iterator into lru_list)
	unordered_map<uint64_t, std::pair<JITCacheEntry, std::list<uint64_t>::iterator>> cache;

	void EvictLRU();
	void TouchEntry(uint64_t fingerprint);
};

} // namespace duckdb
