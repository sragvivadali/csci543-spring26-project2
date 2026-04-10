#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/execution/jit/jit_cache.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"

using namespace duckdb;

namespace {

unique_ptr<BoundComparisonExpression> MakeIntCompare(ExpressionType cmp) {
	return make_uniq<BoundComparisonExpression>(cmp, make_uniq<BoundConstantExpression>(Value::INTEGER(10)),
	                                            make_uniq<BoundConstantExpression>(Value::INTEGER(1)));
}

} // namespace

TEST_CASE("JITDispatcher basic configuration", "[jit_dispatcher]") {
	auto &dispatcher = JITDispatcher::GetInstance();
	
	// Test enable/disable
	dispatcher.SetEnableJIT(true);
	REQUIRE(dispatcher.IsJITEnabled() == true);
	
	dispatcher.SetEnableJIT(false);
	REQUIRE(dispatcher.IsJITEnabled() == false);
	
	// Test threshold configuration
	dispatcher.SetCompilationThreshold(5000);
	REQUIRE(dispatcher.GetCompilationThreshold() == 5000);
	
	dispatcher.SetCompilationThreshold(1000);
	REQUIRE(dispatcher.GetCompilationThreshold() == 1000);
}

TEST_CASE("JITDispatcher statistics tracking", "[jit_dispatcher]") {
	auto &dispatcher = JITDispatcher::GetInstance();
	dispatcher.ResetStats();
	
	auto stats = dispatcher.GetStats();
	REQUIRE(stats.jit_executions == 0);
	REQUIRE(stats.interpreter_executions == 0);
	REQUIRE(stats.compilation_attempts == 0);
}

TEST_CASE("JITDispatcher disabled JIT uses interpreter", "[jit_dispatcher]") {
	auto &dispatcher = JITDispatcher::GetInstance();
	auto &cache = JITCache::GetInstance();
	
	dispatcher.ResetStats();
	dispatcher.SetEnableJIT(false);
	cache.Clear();
	
	auto expr = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);
	
	// Note: TryExecuteJIT requires an ExpressionExecutor which we don't have in this test
	// So we just verify the configuration is correct
	REQUIRE(dispatcher.IsJITEnabled() == false);
	
	auto stats = dispatcher.GetStats();
	REQUIRE(stats.jit_executions == 0);
}

TEST_CASE("JITDispatcher compilation threshold logic", "[jit_dispatcher]") {
	auto &dispatcher = JITDispatcher::GetInstance();
	auto &profiler = JITProfiler::GetInstance();
	
	dispatcher.SetCompilationThreshold(1000);
	profiler.Reset();
	
	auto expr = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);
	
	// Record some executions (below threshold)
	profiler.Record(*expr, 500);
	REQUIRE(profiler.GetCount(*expr) == 500);
	
	// Record more (above threshold)
	profiler.Record(*expr, 600);
	REQUIRE(profiler.GetCount(*expr) == 1100);
	
	// Expression should now be considered hot
	REQUIRE(profiler.GetCount(*expr) >= dispatcher.GetCompilationThreshold());
}

TEST_CASE("JITDispatcher stats reset", "[jit_dispatcher]") {
	auto &dispatcher = JITDispatcher::GetInstance();
	
	dispatcher.ResetStats();
	
	auto stats = dispatcher.GetStats();
	REQUIRE(stats.jit_executions == 0);
	REQUIRE(stats.interpreter_executions == 0);
	REQUIRE(stats.compilation_attempts == 0);
	REQUIRE(stats.compilation_successes == 0);
	REQUIRE(stats.compilation_failures == 0);
}
