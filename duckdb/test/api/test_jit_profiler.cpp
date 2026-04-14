#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/logical_operator.hpp"

#include <functional>
#include <optional>
#include <thread>
#include <vector>

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

//! Bound scalar arithmetic op with INTEGER children (matches DuckDB operator function names).
unique_ptr<BoundFunctionExpression> MakeIntBinaryArith(const string &op) {
	ScalarFunction fn(op, {LogicalType::INTEGER, LogicalType::INTEGER}, LogicalType::INTEGER, nullptr);
	vector<unique_ptr<Expression>> args;
	args.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(10)));
	args.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(1)));
	return make_uniq<BoundFunctionExpression>(LogicalType::INTEGER, std::move(fn), std::move(args), nullptr, true);
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
	auto expr = MakeIntBinaryArith("+");

	auto &prof = JITProfiler::GetInstance();
	prof.Reset();
	prof.Record(*expr, 7);
	prof.Record(*expr, 3);

	REQUIRE(prof.GetCount(*expr) == 10);
}

TEST_CASE("JITProfiler Record counts compilable comparisons and arithmetic", "[jit_profiler]") {
	auto cmp = MakeIntCompare(ExpressionType::COMPARE_GREATERTHAN);
	auto add = MakeIntBinaryArith("+");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();

	REQUIRE_FALSE(JITProfiler::ProfilesAsArithmeticRoot(*cmp));
	REQUIRE(JITProfiler::ProfilesAsArithmeticRoot(*add));

	prof.Record(*cmp, 1000);
	prof.Record(*add, 5);
	REQUIRE(prof.GetCount(*cmp) == 1000);
	REQUIRE(prof.GetCount(*add) == 5);
}

TEST_CASE("JITProfiler per-query scope attributes counts", "[jit_profiler]") {
	auto expr = MakeIntBinaryArith("+");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();

	JITProfiler::PushQueryScope(10);
	prof.Record(*expr, 5);
	JITProfiler::PopQueryScope();

	JITProfiler::PushQueryScope(20);
	prof.Record(*expr, 3);
	JITProfiler::PopQueryScope();

	REQUIRE(prof.GetCount(*expr) == 8);
	REQUIRE(prof.GetCountForQuery(10, *expr) == 5);
	REQUIRE(prof.GetCountForQuery(20, *expr) == 3);
	REQUIRE(prof.GetCountForQuery(99, *expr) == 0);

	auto by_q = prof.GetPerQueryExpressionStats();
	REQUIRE(by_q.size() == 2);
	REQUIRE(by_q[0].query_id == 10);
	REQUIRE(by_q[0].per_expression.size() == 1);
	REQUIRE(by_q[0].per_expression[0].tuple_count == 5);
	REQUIRE(by_q[1].query_id == 20);
	REQUIRE(by_q[1].per_expression[0].tuple_count == 3);
}

TEST_CASE("JITProfiler nested PushQueryScope uses innermost query id", "[jit_profiler]") {
	auto expr = MakeIntBinaryArith("+");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();

	JITProfiler::PushQueryScope(1);
	JITProfiler::PushQueryScope(2);
	prof.Record(*expr, 4);
	JITProfiler::PopQueryScope();
	JITProfiler::PopQueryScope();

	REQUIRE(prof.GetCount(*expr) == 4);
	REQUIRE(prof.GetCountForQuery(1, *expr) == 0);
	REQUIRE(prof.GetCountForQuery(2, *expr) == 4);
}

TEST_CASE("JITProfiler hotness threshold set clear and PassesHotnessThreshold", "[jit_profiler]") {
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();
	REQUIRE_FALSE(prof.HasHotnessThreshold());
	REQUIRE(prof.GetHotnessThreshold() == std::nullopt);
	REQUIRE(prof.GetHotLabeledFingerprints().empty());
	REQUIRE_FALSE(prof.PassesHotnessThreshold(0));
	REQUIRE(prof.PassesHotnessThreshold(1));

	prof.SetHotnessThreshold(100);
	REQUIRE(prof.HasHotnessThreshold());
	REQUIRE(prof.GetHotnessThreshold() == 100);
	REQUIRE_FALSE(prof.PassesHotnessThreshold(99));
	REQUIRE(prof.PassesHotnessThreshold(100));
	REQUIRE(prof.GetHotLabeledFingerprints().empty());

	prof.ClearHotnessThreshold();
	REQUIRE_FALSE(prof.HasHotnessThreshold());
	REQUIRE(prof.PassesHotnessThreshold(1));
}

TEST_CASE("JITProfiler hot labels on Record when threshold configured", "[jit_profiler]") {
	auto expr = MakeIntBinaryArith("+");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();
	prof.SetHotnessThreshold(10);

	const uint64_t fp = JITProfiler::Fingerprint(*expr);
	REQUIRE_FALSE(prof.IsFingerprintHot(fp));
	REQUIRE_FALSE(prof.IsExpressionHot(*expr));

	prof.Record(*expr, 9);
	REQUIRE(prof.GetCount(*expr) == 9);
	REQUIRE_FALSE(prof.IsFingerprintHot(fp));
	REQUIRE(prof.GetHotLabeledFingerprints().empty());

	prof.Record(*expr, 1);
	REQUIRE(prof.GetCount(*expr) == 10);
	REQUIRE(prof.IsFingerprintHot(fp));
	REQUIRE(prof.IsExpressionHot(*expr));
	auto hot = prof.GetHotLabeledFingerprints();
	REQUIRE(hot.size() == 1);
	REQUIRE(hot[0] == fp);

	auto snap = prof.ExportSnapshot();
	REQUIRE(snap.hot_labeled_fingerprints == hot);
}

TEST_CASE("JITProfiler SetHotnessThreshold rescans existing counts into hot labels", "[jit_profiler]") {
	auto expr = MakeIntBinaryArith("+");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();
	prof.Record(*expr, 100);
	const uint64_t fp = JITProfiler::Fingerprint(*expr);

	REQUIRE(prof.IsFingerprintHot(fp));
	REQUIRE(prof.GetHotLabeledFingerprints().empty());

	prof.SetHotnessThreshold(50);
	REQUIRE(prof.IsFingerprintHot(fp));
	auto hot = prof.GetHotLabeledFingerprints();
	REQUIRE(hot.size() == 1);
	REQUIRE(hot[0] == fp);
}

TEST_CASE("JITProfiler IsFingerprintHot without threshold uses any positive count", "[jit_profiler]") {
	auto expr = MakeIntBinaryArith("+");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();
	const uint64_t fp = JITProfiler::Fingerprint(*expr);
	REQUIRE_FALSE(prof.IsFingerprintHot(fp));
	prof.Record(*expr, 1);
	REQUIRE(prof.IsFingerprintHot(fp));
	REQUIRE(prof.GetHotLabeledFingerprints().empty());
}

TEST_CASE("JITProfiler became-hot callback fires once per fingerprint", "[jit_profiler]") {
	auto expr = MakeIntBinaryArith("+");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();
	prof.SetHotnessThreshold(5);

	int calls = 0;
	uint64_t last_fp = 0;
	prof.SetFingerprintBecameHotCallback([&](uint64_t fp) {
		calls++;
		last_fp = fp;
	});

	const uint64_t fp = JITProfiler::Fingerprint(*expr);
	prof.Record(*expr, 5);
	REQUIRE(calls == 1);
	REQUIRE(last_fp == fp);

	prof.Record(*expr, 100);
	REQUIRE(calls == 1);

	prof.ClearFingerprintBecameHotCallback();
	prof.Reset();
	prof.SetHotnessThreshold(1);
	prof.Record(*expr, 10);
	REQUIRE(calls == 1);

	prof.Reset();
}

TEST_CASE("JITProfiler Reset clears stats threshold and overhead", "[jit_profiler]") {
	auto expr = MakeIntBinaryArith("+");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();
	prof.SetHotnessThreshold(50);

	JITProfiler::PushQueryScope(7);
	prof.Record(*expr, 2);
	JITProfiler::PopQueryScope();

	REQUIRE(prof.GetOverheadStats().record_calls >= 1);

	prof.Reset();

	REQUIRE(prof.GetCount(*expr) == 0);
	REQUIRE(prof.GetCountForQuery(7, *expr) == 0);
	REQUIRE_FALSE(prof.HasHotnessThreshold());
	REQUIRE(prof.GetHotLabeledFingerprints().empty());
	auto oh = prof.GetOverheadStats();
	REQUIRE(oh.record_calls == 0);
	REQUIRE(oh.tuples_accounted == 0);
	REQUIRE(oh.locked_section_nanoseconds == 0);
}

TEST_CASE("JITProfiler ExportSnapshot stable ordering", "[jit_profiler]") {
	auto gt = MakeIntBinaryArith("*");
	auto lt = MakeIntBinaryArith("+");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();

	const uint64_t fp_gt = JITProfiler::Fingerprint(*gt);
	const uint64_t fp_lt = JITProfiler::Fingerprint(*lt);

	JITProfiler::PushQueryScope(3);
	prof.Record(*gt, 1);
	prof.Record(*lt, 2);
	JITProfiler::PopQueryScope();

	JITProfiler::PushQueryScope(1);
	prof.Record(*gt, 10);
	JITProfiler::PopQueryScope();

	auto snap = prof.ExportSnapshot();
	REQUIRE(snap.global_by_fingerprint.size() == 2);
	if (fp_lt < fp_gt) {
		REQUIRE(snap.global_by_fingerprint[0].fingerprint == fp_lt);
		REQUIRE(snap.global_by_fingerprint[0].tuple_count == 2);
		REQUIRE(snap.global_by_fingerprint[1].fingerprint == fp_gt);
		REQUIRE(snap.global_by_fingerprint[1].tuple_count == 11);
	} else {
		REQUIRE(snap.global_by_fingerprint[0].fingerprint == fp_gt);
		REQUIRE(snap.global_by_fingerprint[0].tuple_count == 11);
		REQUIRE(snap.global_by_fingerprint[1].fingerprint == fp_lt);
		REQUIRE(snap.global_by_fingerprint[1].tuple_count == 2);
	}

	REQUIRE(snap.by_query.size() == 2);
	REQUIRE(snap.by_query[0].query_id == 1);
	REQUIRE(snap.by_query[1].query_id == 3);
	REQUIRE_FALSE(snap.hotness_threshold_configured);
	REQUIRE(snap.hot_labeled_fingerprints.empty());
	REQUIRE(snap.overhead.record_calls >= 3);
	REQUIRE(snap.overhead.tuples_accounted == 13);
}

TEST_CASE("JITProfiler GetTopHotExpressions ranking and tie-break", "[jit_profiler]") {
	auto a = MakeIntBinaryArith("+");
	auto b = MakeIntBinaryArith("*");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();

	const uint64_t fp_a = JITProfiler::Fingerprint(*a);
	const uint64_t fp_b = JITProfiler::Fingerprint(*b);

	prof.Record(*a, 5);
	prof.Record(*b, 5);
	prof.Record(*a, 5);

	auto top = prof.GetTopHotExpressions(10);
	REQUIRE(top.size() == 2);
	REQUIRE(top[0].tuple_count == 10);
	REQUIRE(top[1].tuple_count == 5);
	if (fp_a < fp_b) {
		REQUIRE(top[1].fingerprint == fp_b);
	} else {
		REQUIRE(top[1].fingerprint == fp_a);
	}
	REQUIRE(top[0].fingerprint == fp_a); // total 10 on a
}

TEST_CASE("JITProfiler GetTopHotExpressions respects limit", "[jit_profiler]") {
	auto a = MakeIntBinaryArith("+");
	auto b = MakeIntBinaryArith("*");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();
	prof.Record(*a, 100);
	prof.Record(*b, 1);
	auto top = prof.GetTopHotExpressions(1);
	REQUIRE(top.size() == 1);
	REQUIRE(top[0].tuple_count == 100);
}

TEST_CASE("JITProfiler GetGlobalExpressionStats matches aggregate counts", "[jit_profiler]") {
	auto expr = MakeIntBinaryArith("+");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();
	prof.Record(*expr, 9);
	auto rows = prof.GetGlobalExpressionStats();
	REQUIRE(rows.size() == 1);
	REQUIRE(rows[0].fingerprint == JITProfiler::Fingerprint(*expr));
	REQUIRE(rows[0].tuple_count == 9);
}

TEST_CASE("JITProfiler concurrent Record is safe", "[jit_profiler]") {
	auto expr = MakeIntBinaryArith("+");
	auto &prof = JITProfiler::GetInstance();
	prof.Reset();

	constexpr int kThreads = 4;
	constexpr int kIters = 500;
	vector<std::thread> threads;
	threads.reserve(kThreads);
	for (int t = 0; t < kThreads; t++) {
		threads.emplace_back([&prof, raw = expr.get()] {
			for (int i = 0; i < kIters; i++) {
				prof.Record(*raw, 1);
			}
		});
	}
	for (auto &th : threads) {
		th.join();
	}

	REQUIRE(prof.GetCount(*expr) == idx_t(kThreads) * idx_t(kIters));
}
