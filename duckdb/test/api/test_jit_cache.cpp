#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/execution/jit/jit_cache.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/common/vector.hpp"

using namespace duckdb;

namespace {

unique_ptr<BoundComparisonExpression> MakeIntCompare(ExpressionType cmp) {
	return make_uniq<BoundComparisonExpression>(cmp, make_uniq<BoundConstantExpression>(Value::INTEGER(10)),
	                                            make_uniq<BoundConstantExpression>(Value::INTEGER(1)));
}

// Dummy JIT function for testing
void DummyJITFunction(Vector **inputs, Vector &result, idx_t count) {
	// Does nothing - just for testing cache mechanics
	(void)inputs;
	(void)result;
	(void)count;
}

} // namespace

TEST_CASE("JITCache basic insert and lookup", "[jit_cache]") {
	auto &cache = JITCache::GetInstance();
	cache.Clear();

	auto expr = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);
	
	// Initially not in cache
	auto func = cache.Lookup(*expr);
	REQUIRE(func == nullptr);

	// Insert function
	cache.Insert(*expr, DummyJITFunction, 1000);

	// Should now be found
	func = cache.Lookup(*expr);
	REQUIRE(func != nullptr);

	// Stats should show 1 hit, 1 miss
	auto stats = cache.GetStats();
	REQUIRE(stats.total_hits == 1);
	REQUIRE(stats.total_misses == 1);
	REQUIRE(stats.total_entries == 1);
}

TEST_CASE("JITCache LRU eviction", "[jit_cache]") {
	auto &cache = JITCache::GetInstance();
	cache.Clear();
	cache.SetMaxEntries(3);

	// Create 4 different expressions
	auto expr1 = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);
	auto expr2 = MakeIntCompare(ExpressionType::COMPARE_LESSTHAN);
	auto expr3 = MakeIntCompare(ExpressionType::COMPARE_EQUAL);
	auto expr4 = MakeIntCompare(ExpressionType::COMPARE_NOTEQUAL);

	// Insert first 3
	cache.Insert(*expr1, DummyJITFunction, 100);
	cache.Insert(*expr2, DummyJITFunction, 200);
	cache.Insert(*expr3, DummyJITFunction, 300);

	REQUIRE(cache.GetStats().total_entries == 3);

	// Insert 4th - should evict expr1 (LRU)
	cache.Insert(*expr4, DummyJITFunction, 400);

	REQUIRE(cache.GetStats().total_entries == 3);
	REQUIRE(cache.GetStats().total_evictions == 1);

	// expr1 should be evicted
	REQUIRE(cache.Lookup(*expr1) == nullptr);
	
	// Others should still be present
	REQUIRE(cache.Lookup(*expr2) != nullptr);
	REQUIRE(cache.Lookup(*expr3) != nullptr);
	REQUIRE(cache.Lookup(*expr4) != nullptr);
}

TEST_CASE("JITCache LRU ordering with access", "[jit_cache]") {
	auto &cache = JITCache::GetInstance();
	cache.Clear();
	cache.SetMaxEntries(3);

	auto expr1 = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);
	auto expr2 = MakeIntCompare(ExpressionType::COMPARE_LESSTHAN);
	auto expr3 = MakeIntCompare(ExpressionType::COMPARE_EQUAL);
	auto expr4 = MakeIntCompare(ExpressionType::COMPARE_NOTEQUAL);

	// Insert first 3
	cache.Insert(*expr1, DummyJITFunction, 100);
	cache.Insert(*expr2, DummyJITFunction, 200);
	cache.Insert(*expr3, DummyJITFunction, 300);

	// Access expr1 to make it recently used
	cache.Lookup(*expr1);

	// Insert expr4 - should evict expr2 (now LRU) instead of expr1
	cache.Insert(*expr4, DummyJITFunction, 400);

	// expr1 should still be present (was accessed)
	REQUIRE(cache.Lookup(*expr1) != nullptr);
	
	// expr2 should be evicted
	REQUIRE(cache.Lookup(*expr2) == nullptr);
	
	// expr3 and expr4 should be present
	REQUIRE(cache.Lookup(*expr3) != nullptr);
	REQUIRE(cache.Lookup(*expr4) != nullptr);
}

TEST_CASE("JITCache clear", "[jit_cache]") {
	auto &cache = JITCache::GetInstance();
	cache.Clear();

	auto expr1 = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);
	auto expr2 = MakeIntCompare(ExpressionType::COMPARE_LESSTHAN);

	cache.Insert(*expr1, DummyJITFunction, 100);
	cache.Insert(*expr2, DummyJITFunction, 200);

	REQUIRE(cache.GetStats().total_entries == 2);

	cache.Clear();

	REQUIRE(cache.GetStats().total_entries == 0);
	REQUIRE(cache.Lookup(*expr1) == nullptr);
	REQUIRE(cache.Lookup(*expr2) == nullptr);
}

TEST_CASE("JITCache duplicate insert ignored", "[jit_cache]") {
	auto &cache = JITCache::GetInstance();
	cache.Clear();

	auto expr = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);

	cache.Insert(*expr, DummyJITFunction, 100);
	REQUIRE(cache.GetStats().total_entries == 1);

	// Insert same expression again - should be ignored
	cache.Insert(*expr, DummyJITFunction, 200);
	REQUIRE(cache.GetStats().total_entries == 1);
}

TEST_CASE("JITCache SetMaxEntries triggers eviction", "[jit_cache]") {
	auto &cache = JITCache::GetInstance();
	cache.Clear();
	cache.SetMaxEntries(10);

	// Insert 5 different expression types
	auto expr1 = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);
	auto expr2 = MakeIntCompare(ExpressionType::COMPARE_LESSTHAN);
	auto expr3 = MakeIntCompare(ExpressionType::COMPARE_EQUAL);
	auto expr4 = MakeIntCompare(ExpressionType::COMPARE_NOTEQUAL);
	auto expr5 = MakeIntCompare(ExpressionType::COMPARE_GREATERTHANOREQUALTO);
	
	cache.Insert(*expr1, DummyJITFunction, 100);
	cache.Insert(*expr2, DummyJITFunction, 100);
	cache.Insert(*expr3, DummyJITFunction, 100);
	cache.Insert(*expr4, DummyJITFunction, 100);
	cache.Insert(*expr5, DummyJITFunction, 100);

	REQUIRE(cache.GetStats().total_entries == 5);

	// Reduce max to 3 - should evict 2
	cache.SetMaxEntries(3);

	REQUIRE(cache.GetStats().total_entries == 3);
	REQUIRE(cache.GetStats().total_evictions == 2);
}
