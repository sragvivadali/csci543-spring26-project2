//===----------------------------------------------------------------------===//
// test/api/test_jit_compiler.cpp
//
// CSCI 543 — Unit tests for JITCompiler (Person B)
//
// Tests are grouped into:
//   [jit_compiler_can_compile]   — CanCompile() structural checks (no LLVM needed)
//   [jit_compiler_exec]          — Compile() + run correctness (requires LLVM)
//   [jit_compiler_nulls]         — NULL propagation correctness
//   [jit_compiler_types]         — Type promotion rules
//   [jit_compiler_ir]            — LLVM IR content checks
//
// Run all: ./test/unittest "[jit_compiler]"
// Run one group: ./test/unittest "[jit_compiler_exec]"
//===----------------------------------------------------------------------===//

#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/execution/jit/jit_compiler.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/function/scalar_function.hpp"

#include <cstring>
#include <vector>
#include <cmath>

using namespace duckdb;

// ── Expression construction helpers ─────────────────────────────────────────

/// Make a BoundReferenceExpression (column slot reference).
static unique_ptr<Expression> MakeColRef(LogicalType type, idx_t col_idx) {
	return make_uniq<BoundReferenceExpression>(std::move(type), (storage_t)col_idx);
}

/// Make an integer constant expression.
static unique_ptr<Expression> MakeIntConst(int32_t v) {
	return make_uniq<BoundConstantExpression>(Value::INTEGER(v));
}

/// Make a double constant expression.
static unique_ptr<Expression> MakeDblConst(double v) {
	return make_uniq<BoundConstantExpression>(Value::DOUBLE(v));
}

/// Make a NULL constant of the given type.
static unique_ptr<Expression> MakeNullConst(LogicalType type) {
	return make_uniq<BoundConstantExpression>(Value(std::move(type)));
}

/// Make a binary arithmetic BoundFunctionExpression.
/// function_name should be "+", "-", "*", or "/".
static unique_ptr<Expression> MakeArith(std::string fn_name,
                                        unique_ptr<Expression> lhs,
                                        unique_ptr<Expression> rhs,
                                        LogicalType return_type) {
	ScalarFunction fn(fn_name,
	                  {lhs->return_type, rhs->return_type},
	                  return_type,
	                  /*function=*/nullptr);
	vector<unique_ptr<Expression>> args;
	args.push_back(std::move(lhs));
	args.push_back(std::move(rhs));
	return make_uniq<BoundFunctionExpression>(return_type, std::move(fn), std::move(args),
	                                          /*bind_info=*/nullptr, /*is_operator=*/true);
}

/// Make a BoundComparisonExpression.
static unique_ptr<Expression> MakeCmp(ExpressionType op,
                                      unique_ptr<Expression> lhs,
                                      unique_ptr<Expression> rhs) {
	LogicalType ret = LogicalType::BOOLEAN;
	return make_uniq<BoundComparisonExpression>(op, std::move(lhs), std::move(rhs));
}

// ── Helper: run JIT-compiled fn over flat int32 arrays ───────────────────────

struct TestRunI32 {
	std::vector<int32_t> a_data, b_data;
	std::vector<uint8_t> a_valid, b_valid;
	std::vector<int32_t> result_data;
	std::vector<uint8_t> result_valid;
	idx_t count;

	TestRunI32(std::vector<int32_t> a, std::vector<int32_t> b,
	           std::vector<uint8_t> av, std::vector<uint8_t> bv)
	    : a_data(std::move(a)), b_data(std::move(b)),
	      a_valid(std::move(av)), b_valid(std::move(bv)),
	      count(a_data.size()) {
		result_data.resize(count, 0);
		result_valid.resize(count, 0);
	}

	/// Call fn with two-column integer input.
	void Run(JITCompiledFn fn) {
		void *col_data_ptrs[2] = {a_data.data(), b_data.data()};
		uint8_t *col_valid_ptrs[2] = {a_valid.data(), b_valid.data()};
		fn(col_data_ptrs, col_valid_ptrs, result_data.data(), result_valid.data(), count);
	}
};

// ============================================================================
// CanCompile tests — purely structural, no LLVM required
// ============================================================================

TEST_CASE("JITCompiler CanCompile: constant int comparison is compilable",
          "[jit_compiler][jit_compiler_can_compile]") {
	auto expr = MakeCmp(ExpressionType::COMPARE_GREATERTHAN,
	                    MakeIntConst(10), MakeIntConst(5));
	REQUIRE(JITCompiler::GetInstance().CanCompile(*expr));
}

TEST_CASE("JITCompiler CanCompile: column ref + constant add is compilable",
          "[jit_compiler][jit_compiler_can_compile]") {
	auto expr = MakeArith("+", MakeColRef(LogicalType::INTEGER, 0),
	                      MakeIntConst(1), LogicalType::INTEGER);
	REQUIRE(JITCompiler::GetInstance().CanCompile(*expr));
}

TEST_CASE("JITCompiler CanCompile: double arithmetic is compilable",
          "[jit_compiler][jit_compiler_can_compile]") {
	auto expr = MakeArith("*", MakeColRef(LogicalType::DOUBLE, 0),
	                      MakeDblConst(2.0), LogicalType::DOUBLE);
	REQUIRE(JITCompiler::GetInstance().CanCompile(*expr));
}

TEST_CASE("JITCompiler CanCompile: AND of two comparisons is compilable",
          "[jit_compiler][jit_compiler_can_compile]") {
	auto cmp1 = MakeCmp(ExpressionType::COMPARE_GREATERTHAN, MakeIntConst(3), MakeIntConst(1));
	auto cmp2 = MakeCmp(ExpressionType::COMPARE_LESSTHAN,    MakeIntConst(3), MakeIntConst(9));

	auto conj = make_uniq<BoundConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, std::move(cmp1), std::move(cmp2));
	REQUIRE(JITCompiler::GetInstance().CanCompile(*conj));
}

TEST_CASE("JITCompiler CanCompile: VARCHAR constant is NOT compilable",
          "[jit_compiler][jit_compiler_can_compile]") {
	// VARCHAR has PhysicalType::VARCHAR — not in our supported set.
	auto cnst = make_uniq<BoundConstantExpression>(Value("hello"));
	REQUIRE_FALSE(JITCompiler::GetInstance().CanCompile(*cnst));
}

TEST_CASE("JITCompiler CanCompile: NULL constant of supported type is compilable",
          "[jit_compiler][jit_compiler_can_compile]") {
	auto cnst = MakeNullConst(LogicalType::INTEGER);
	REQUIRE(JITCompiler::GetInstance().CanCompile(*cnst));
}

TEST_CASE("JITCompiler CanCompile: integer column ref is compilable",
          "[jit_compiler][jit_compiler_can_compile]") {
	auto ref = MakeColRef(LogicalType::INTEGER, 0);
	REQUIRE(JITCompiler::GetInstance().CanCompile(*ref));
}

// ============================================================================
// Execution correctness tests — require DUCKDB_JIT_ENABLED
// Skipped gracefully when LLVM is not linked.
// ============================================================================

TEST_CASE("JITCompiler exec: constant comparison 10 > 5 returns true for all rows",
          "[jit_compiler][jit_compiler_exec]") {
	auto &jit = JITCompiler::GetInstance();
	auto expr = MakeCmp(ExpressionType::COMPARE_GREATERTHAN,
	                    MakeIntConst(10), MakeIntConst(5));

	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping exec test: LLVM JIT not available (build without -DDUCKDB_JIT_ENABLED)");
		return;
	}

	// No column refs → col_data / col_valid arrays can be empty.
	const idx_t count = 4;
	uint8_t result_data[count];
	uint8_t result_valid[count];
	std::memset(result_data, 0, count);
	std::memset(result_valid, 0, count);

	void *col_data_ptrs[1] = {};
	uint8_t *col_valid_ptrs[1] = {};
	fn(col_data_ptrs, col_valid_ptrs, result_data, result_valid, count);

	for (idx_t i = 0; i < count; i++) {
		INFO("row " << i);
		REQUIRE(result_valid[i] == 1);  // not NULL
		REQUIRE(result_data[i] == 1);   // true (10 > 5)
	}
}

TEST_CASE("JITCompiler exec: constant comparison 3 < 2 returns false for all rows",
          "[jit_compiler][jit_compiler_exec]") {
	auto &jit = JITCompiler::GetInstance();
	auto expr = MakeCmp(ExpressionType::COMPARE_LESSTHAN,
	                    MakeIntConst(3), MakeIntConst(2));
	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	const idx_t count = 3;
	uint8_t result_data[count];
	uint8_t result_valid[count];
	void *cdp[1] = {};
	uint8_t *cvp[1] = {};
	fn(cdp, cvp, result_data, result_valid, count);

	for (idx_t i = 0; i < count; i++) {
		REQUIRE(result_valid[i] == 1);
		REQUIRE(result_data[i] == 0);  // false
	}
}

TEST_CASE("JITCompiler exec: integer column addition col[0] + col[1]",
          "[jit_compiler][jit_compiler_exec]") {
	auto &jit = JITCompiler::GetInstance();
	// col[0] + col[1] → INTEGER
	auto expr = MakeArith("+", MakeColRef(LogicalType::INTEGER, 0),
	                      MakeColRef(LogicalType::INTEGER, 1), LogicalType::INTEGER);
	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	TestRunI32 t({1, 2, 3, 4}, {10, 20, 30, 40},
	             {1, 1, 1, 1}, {1, 1, 1, 1});
	t.Run(fn);

	REQUIRE(t.result_valid[0] == 1); REQUIRE(t.result_data[0] == 11);
	REQUIRE(t.result_valid[1] == 1); REQUIRE(t.result_data[1] == 22);
	REQUIRE(t.result_valid[2] == 1); REQUIRE(t.result_data[2] == 33);
	REQUIRE(t.result_valid[3] == 1); REQUIRE(t.result_data[3] == 44);
}

TEST_CASE("JITCompiler exec: integer subtraction col[0] - col[1]",
          "[jit_compiler][jit_compiler_exec]") {
	auto &jit = JITCompiler::GetInstance();
	auto expr = MakeArith("-", MakeColRef(LogicalType::INTEGER, 0),
	                      MakeColRef(LogicalType::INTEGER, 1), LogicalType::INTEGER);
	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	TestRunI32 t({100, 50}, {30, 80}, {1, 1}, {1, 1});
	t.Run(fn);

	REQUIRE(t.result_data[0] == 70);
	REQUIRE(t.result_data[1] == -30);
}

TEST_CASE("JITCompiler exec: double multiplication col[0] * constant",
          "[jit_compiler][jit_compiler_exec]") {
	auto &jit = JITCompiler::GetInstance();
	auto expr = MakeArith("*", MakeColRef(LogicalType::DOUBLE, 0),
	                      MakeDblConst(2.5), LogicalType::DOUBLE);
	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	const idx_t count = 3;
	double input_data[] = {1.0, 2.0, 4.0};
	uint8_t input_valid[] = {1, 1, 1};
	double result_data[count];
	uint8_t result_valid[count];

	void *cdp[1] = {input_data};
	uint8_t *cvp[1] = {input_valid};
	fn(cdp, cvp, result_data, result_valid, count);

	REQUIRE(result_valid[0] == 1); REQUIRE(result_data[0] == Approx(2.5));
	REQUIRE(result_valid[1] == 1); REQUIRE(result_data[1] == Approx(5.0));
	REQUIRE(result_valid[2] == 1); REQUIRE(result_data[2] == Approx(10.0));
}

// ============================================================================
// NULL propagation tests
// ============================================================================

TEST_CASE("JITCompiler nulls: NULL input propagates to output",
          "[jit_compiler][jit_compiler_nulls]") {
	auto &jit = JITCompiler::GetInstance();
	auto expr = MakeArith("+", MakeColRef(LogicalType::INTEGER, 0),
	                      MakeColRef(LogicalType::INTEGER, 1), LogicalType::INTEGER);
	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	// Row 0: both valid  → result valid
	// Row 1: col[0] NULL → result NULL
	// Row 2: col[1] NULL → result NULL
	// Row 3: both valid  → result valid
	TestRunI32 t({1, 2, 3, 4}, {10, 20, 30, 40},
	             {1, 0, 1, 1}, {1, 1, 0, 1});
	t.Run(fn);

	REQUIRE(t.result_valid[0] == 1); REQUIRE(t.result_data[0] == 11);
	REQUIRE(t.result_valid[1] == 0); // NULL
	REQUIRE(t.result_valid[2] == 0); // NULL
	REQUIRE(t.result_valid[3] == 1); REQUIRE(t.result_data[3] == 44);
}

TEST_CASE("JITCompiler nulls: NULL constant expression produces NULL result",
          "[jit_compiler][jit_compiler_nulls]") {
	auto &jit = JITCompiler::GetInstance();
	auto expr = MakeNullConst(LogicalType::INTEGER);
	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	const idx_t count = 2;
	int32_t result_data[count] = {99, 99};
	uint8_t result_valid[count] = {99, 99};

	void *cdp[1] = {};
	uint8_t *cvp[1] = {};
	fn(cdp, cvp, result_data, result_valid, count);

	REQUIRE(result_valid[0] == 0); // NULL
	REQUIRE(result_valid[1] == 0); // NULL
}

TEST_CASE("JITCompiler nulls: comparison with NULL operand produces NULL",
          "[jit_compiler][jit_compiler_nulls]") {
	auto &jit = JITCompiler::GetInstance();
	// col[0] > 5  where col[0] might be NULL
	auto expr = MakeCmp(ExpressionType::COMPARE_GREATERTHAN,
	                    MakeColRef(LogicalType::INTEGER, 0), MakeIntConst(5));
	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	int32_t col_data[]    = {10, 3};     // row 0 > 5 = true, row 1 > 5 = false
	uint8_t col_valid[]   = {1,  0};     // row 1 is NULL input
	uint8_t result_data[2];
	uint8_t result_valid[2];

	void *cdp[1] = {col_data};
	uint8_t *cvp[1] = {col_valid};
	fn(cdp, cvp, result_data, result_valid, 2);

	REQUIRE(result_valid[0] == 1);  // 10 > 5 → true (valid)
	REQUIRE(result_data[0]  == 1);
	REQUIRE(result_valid[1] == 0);  // NULL input → NULL result
}

// ============================================================================
// Type promotion tests
// ============================================================================

TEST_CASE("JITCompiler types: INT32 column vs INT64 constant promoted to INT64",
          "[jit_compiler][jit_compiler_types]") {
	auto &jit = JITCompiler::GetInstance();
	// col[0] (INTEGER=INT32) + bigint_constant (BIGINT=INT64) → BIGINT
	auto bigint_const = make_uniq<BoundConstantExpression>(Value::BIGINT(1000000000LL));
	auto expr = MakeArith("+",
	                      MakeColRef(LogicalType::INTEGER, 0),
	                      std::move(bigint_const),
	                      LogicalType::BIGINT);
	REQUIRE(jit.CanCompile(*expr));

	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	int32_t col_data[]   = {1, 2};
	uint8_t col_valid[]  = {1, 1};
	int64_t result_data[2] = {};
	uint8_t result_valid[2] = {};

	void *cdp[1] = {col_data};
	uint8_t *cvp[1] = {col_valid};
	fn(cdp, cvp, result_data, result_valid, 2);

	REQUIRE(result_valid[0] == 1);
	REQUIRE(result_data[0]  == 1000000001LL);
	REQUIRE(result_valid[1] == 1);
	REQUIRE(result_data[1]  == 1000000002LL);
}

TEST_CASE("JITCompiler types: DOUBLE + INTEGER promoted to DOUBLE",
          "[jit_compiler][jit_compiler_types]") {
	auto &jit = JITCompiler::GetInstance();
	// col[0] (DOUBLE) + col[1] (INTEGER) → DOUBLE
	auto expr = MakeArith("+",
	                      MakeColRef(LogicalType::DOUBLE, 0),
	                      MakeColRef(LogicalType::INTEGER, 1),
	                      LogicalType::DOUBLE);
	REQUIRE(jit.CanCompile(*expr));

	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	double  a_data[]   = {1.5, 2.5};
	int32_t b_data[]   = {10,  20};
	uint8_t a_valid[]  = {1,   1};
	uint8_t b_valid[]  = {1,   1};
	double  result_data[2] = {};
	uint8_t result_valid[2] = {};

	void *cdp[2] = {a_data, b_data};
	uint8_t *cvp[2] = {a_valid, b_valid};
	fn(cdp, cvp, result_data, result_valid, 2);

	REQUIRE(result_valid[0] == 1); REQUIRE(result_data[0] == Approx(11.5));
	REQUIRE(result_valid[1] == 1); REQUIRE(result_data[1] == Approx(22.5));
}

TEST_CASE("JITCompiler types: FLOAT column cast to DOUBLE",
          "[jit_compiler][jit_compiler_types]") {
	auto &jit = JITCompiler::GetInstance();
	// CAST(col[0] AS DOUBLE)
	auto child = MakeColRef(LogicalType::FLOAT, 0);
	// BoundCastExpression requires a BoundCastInfo — use default numeric cast
	BoundCastInfo cast_info(nullptr, nullptr, nullptr);
	auto cast_expr = make_uniq<BoundCastExpression>(std::move(child),
	                                                LogicalType::DOUBLE,
	                                                std::move(cast_info),
	                                                /*try_cast=*/false);
	REQUIRE(jit.CanCompile(*cast_expr));

	auto fn = jit.Compile(*cast_expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	float   input_data[]   = {1.5f, 3.0f};
	uint8_t input_valid[]  = {1, 1};
	double  result_data[2] = {};
	uint8_t result_valid[2] = {};

	void *cdp[1] = {input_data};
	uint8_t *cvp[1] = {input_valid};
	fn(cdp, cvp, result_data, result_valid, 2);

	REQUIRE(result_valid[0] == 1); REQUIRE(result_data[0] == Approx(1.5));
	REQUIRE(result_valid[1] == 1); REQUIRE(result_data[1] == Approx(3.0));
}

// ============================================================================
// IR content tests (require LLVM)
// ============================================================================

TEST_CASE("JITCompiler IR: compiled IR is non-empty and contains function name",
          "[jit_compiler][jit_compiler_ir]") {
	auto &jit = JITCompiler::GetInstance();
	auto expr = MakeArith("+", MakeColRef(LogicalType::INTEGER, 0),
	                      MakeColRef(LogicalType::INTEGER, 1), LogicalType::INTEGER);
	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	const std::string &ir = jit.GetLastIR();
	REQUIRE_FALSE(ir.empty());
	// IR should define our function
	REQUIRE(ir.find("jit_expr_") != std::string::npos);
	// IR should contain a loop (via the 'loop_header' label)
	REQUIRE(ir.find("loop_header") != std::string::npos);
}

TEST_CASE("JITCompiler IR: integer add produces add instruction in IR",
          "[jit_compiler][jit_compiler_ir]") {
	auto &jit = JITCompiler::GetInstance();
	auto expr = MakeArith("+", MakeColRef(LogicalType::INTEGER, 0),
	                      MakeColRef(LogicalType::INTEGER, 1), LogicalType::INTEGER);
	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	const std::string &ir = jit.GetLastIR();
	// Should contain an "add" instruction for integer addition.
	REQUIRE(ir.find(" add ") != std::string::npos);
}

TEST_CASE("JITCompiler IR: double add produces fadd instruction in IR",
          "[jit_compiler][jit_compiler_ir]") {
	auto &jit = JITCompiler::GetInstance();
	auto expr = MakeArith("+", MakeColRef(LogicalType::DOUBLE, 0),
	                      MakeColRef(LogicalType::DOUBLE, 1), LogicalType::DOUBLE);
	auto fn = jit.Compile(*expr);
	if (!fn) {
		WARN("Skipping: LLVM JIT not available");
		return;
	}

	const std::string &ir = jit.GetLastIR();
	REQUIRE(ir.find("fadd") != std::string::npos);
}

// ============================================================================
// Fallback / unsupported expression tests
// ============================================================================

TEST_CASE("JITCompiler fallback: CanCompile returns false for VARCHAR column",
          "[jit_compiler][jit_compiler_can_compile]") {
	auto ref = MakeColRef(LogicalType::VARCHAR, 0);
	REQUIRE_FALSE(JITCompiler::GetInstance().CanCompile(*ref));
}

TEST_CASE("JITCompiler fallback: Compile returns nullptr for unsupported expression",
          "[jit_compiler][jit_compiler_can_compile]") {
	// VARCHAR ref — CanCompile returns false, so Compile must return nullptr.
	auto ref = MakeColRef(LogicalType::VARCHAR, 0);
	auto fn = JITCompiler::GetInstance().Compile(*ref);
	REQUIRE(fn == nullptr);
}

TEST_CASE("JITCompiler fallback: CanCompile returns false for unknown function name",
          "[jit_compiler][jit_compiler_can_compile]") {
	// Use an unsupported function name like "some_udf"
	auto expr = MakeArith("some_udf", MakeColRef(LogicalType::INTEGER, 0),
	                       MakeIntConst(1), LogicalType::INTEGER);
	REQUIRE_FALSE(JITCompiler::GetInstance().CanCompile(*expr));
}
