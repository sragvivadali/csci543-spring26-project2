//===----------------------------------------------------------------------===//
// JITCache — LRU map fingerprint → JITCompiledFunction. Keys match JITProfiler::Fingerprint.
// Thread-safe; used by JITDispatcher after successful compilation (when wired).
//===----------------------------------------------------------------------===//
#include "duckdb/execution/jit/jit_cache.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"

#include <iostream>

namespace duckdb {

JITCache &JITCache::GetInstance() {
	static JITCache instance;
	return instance;
}

JITCompiledFunction JITCache::Lookup(const Expression &expr) {
	const uint64_t fp = JITProfiler::Fingerprint(expr);
	std::lock_guard<std::mutex> guard(lock);
	
	auto it = cache.find(fp);
	if (it == cache.end()) {
		total_misses++;
		return nullptr;
	}
	
	total_hits++;
	it->second.first.hit_count++;
	TouchEntry(fp);
	return it->second.first.function;
}

void JITCache::Insert(const Expression &expr, JITCompiledFunction function, idx_t compile_time_us) {
	const uint64_t fp = JITProfiler::Fingerprint(expr);
	std::lock_guard<std::mutex> guard(lock);
	
	// Check if already exists
	if (cache.find(fp) != cache.end()) {
		return;
	}
	
	// Evict if at capacity
	if (cache.size() >= max_entries) {
		EvictLRU();
	}
	
	// Insert at front of LRU list
	lru_list.push_front(fp);
	auto list_it = lru_list.begin();
	
	// Create cache entry
	JITCacheEntry entry(std::move(function), fp, compile_time_us);
	cache.insert({fp, {std::move(entry), list_it}});
}

void JITCache::Clear() {
	std::lock_guard<std::mutex> guard(lock);
	cache.clear();
	lru_list.clear();
	total_hits = 0;
	total_misses = 0;
	total_evictions = 0;
}

JITCache::CacheStats JITCache::GetStats() const {
	std::lock_guard<std::mutex> guard(lock);
	return {cache.size(), total_hits, total_misses, total_evictions, max_entries};
}

void JITCache::SetMaxEntries(idx_t max) {
	std::lock_guard<std::mutex> guard(lock);
	max_entries = max;
	
	// Evict entries if we're now over capacity
	while (cache.size() > max_entries) {
		EvictLRU();
	}
}

void JITCache::PrintCache() const {
	std::lock_guard<std::mutex> guard(lock);
	std::cout << "=== JIT Cache Contents ===" << std::endl;
	std::cout << "Total entries: " << cache.size() << "/" << max_entries << std::endl;
	std::cout << "Total hits: " << total_hits << std::endl;
	std::cout << "Total misses: " << total_misses << std::endl;
	std::cout << "Total evictions: " << total_evictions << std::endl;
	
	if (!cache.empty()) {
		std::cout << "\nCached expressions (LRU order):" << std::endl;
		for (const auto &fp : lru_list) {
			auto it = cache.find(fp);
			if (it != cache.end()) {
				const auto &entry = it->second.first;
				std::cout << "  Fingerprint " << entry.fingerprint 
				          << " | hits: " << entry.hit_count
				          << " | compile_time: " << entry.compile_time_us << "us" << std::endl;
			}
		}
	}
}

void JITCache::EvictLRU() {
	if (lru_list.empty()) {
		return;
	}
	
	// Remove least recently used (back of list)
	uint64_t evict_fp = lru_list.back();
	lru_list.pop_back();
	cache.erase(evict_fp);
	total_evictions++;
}

void JITCache::TouchEntry(uint64_t fingerprint) {
	auto it = cache.find(fingerprint);
	if (it == cache.end()) {
		return;
	}
	
	// Move to front of LRU list
	auto list_it = it->second.second;
	lru_list.erase(list_it);
	lru_list.push_front(fingerprint);
	it->second.second = lru_list.begin();
}

} // namespace duckdb
