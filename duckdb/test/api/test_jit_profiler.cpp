#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"

using namespace duckdb;

namespace {

void CollectExpressionsRecursive(LogicalOperator &op, vector<Expression *> &out) {
	for (auto &e : op.expressions) {
		out.push_back(e.get());
	}
	for (auto &child : op.children) {
		CollectExpressionsRecursive(*child, out);
	}
}

vector<Expression *> CollectExpressions(LogicalOperator &root) {
	vector<Expression *> out;
	CollectExpressionsRecursive(root, out);
	return out;
}

unique_ptr<BoundComparisonExpression> MakeIntCompare(ExpressionType cmp) {
	return make_uniq<BoundComparisonExpression>(cmp, make_uniq<BoundConstantExpression>(Value::INTEGER(10)),
	                                            make_uniq<BoundConstantExpression>(Value::INTEGER(1)));
}

} // namespace

TEST_CASE("JITProfiler fingerprint: identical comparison trees match", "[jit_profiler]") {
	auto a = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);
	auto b = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);
	REQUIRE(JITProfiler::Fingerprint(*a) == JITProfiler::Fingerprint(*b));
}

TEST_CASE("JITProfiler fingerprint: comparison type affects fingerprint", "[jit_profiler]") {
	auto gt = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);
	auto eq = MakeIntCompare(ExpressionType::COMPARE_EQUAL);
	REQUIRE(JITProfiler::Fingerprint(*gt) != JITProfiler::Fingerprint(*eq));
}

TEST_CASE("JITProfiler fingerprint: Copy() preserves fingerprint", "[jit_profiler]") {
	auto orig = MakeIntCompare(ExpressionType::COMPARE_LESSTHANOREQUALTO);
	auto copy = orig->Copy();
	REQUIRE(JITProfiler::Fingerprint(*orig) == JITProfiler::Fingerprint(*copy));
}

TEST_CASE("JITProfiler fingerprint: constant payload not hashed (same FP, different values)", "[jit_profiler]") {
	auto x = make_uniq<BoundConstantExpression>(Value::INTEGER(1));
	auto y = make_uniq<BoundConstantExpression>(Value::INTEGER(999));
	INFO("Structural hash ignores constant Value bits; same logical/physical type => same fingerprint");
	REQUIRE(JITProfiler::Fingerprint(*x) == JITProfiler::Fingerprint(*y));
}

TEST_CASE("JITProfiler fingerprint: same ExtractPlan twice matches pairwise", "[jit_profiler]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE jit_fp_t(a DOUBLE, b DOUBLE);"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO jit_fp_t SELECT random(), random() FROM range(3);"));

	auto plan1 = con.ExtractPlan("SELECT a * b FROM jit_fp_t WHERE a * 1.1 > 50");
	auto plan2 = con.ExtractPlan("SELECT a * b FROM jit_fp_t WHERE a * 1.1 > 50");

	auto ex1 = CollectExpressions(*plan1);
	auto ex2 = CollectExpressions(*plan2);
	REQUIRE(ex1.size() == ex2.size());
	for (idx_t i = 0; i < ex1.size(); i++) {
		REQUIRE(JITProfiler::Fingerprint(*ex1[i]) == JITProfiler::Fingerprint(*ex2[i]));
	}
}

TEST_CASE("JITProfiler fingerprint: ExtractPlan yields bound expressions", "[jit_profiler]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE jit_fp_t3(a DOUBLE);"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO jit_fp_t3 VALUES (3.0);"));

	auto plan = con.ExtractPlan("SELECT a * 2.0 FROM jit_fp_t3 WHERE a * 2.0 > 1.0");
	auto exprs = CollectExpressions(*plan);
	REQUIRE(!exprs.empty());
	for (auto *e : exprs) {
		(void)JITProfiler::Fingerprint(*e);
	}
}

TEST_CASE("JITProfiler Record aggregates with same key as Fingerprint", "[jit_profiler]") {
	auto expr = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);

	auto &prof = JITProfiler::GetInstance();
	prof.Reset();
	prof.Record(*expr, 7);
	prof.Record(*expr, 3);

	REQUIRE(prof.GetCount(*expr) == 10);
}
