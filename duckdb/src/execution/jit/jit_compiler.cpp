//===----------------------------------------------------------------------===//
// duckdb/execution/jit/jit_compiler.cpp
//
// CSCI 543 — Person B: LLVM JIT Compiler + Type Support + Fallback Logic
//
// Build with LLVM:
//   cmake -DDUCKDB_JIT_ENABLED=ON ...
//   (see src/execution/CMakeLists.txt for LLVM linkage)
//
// Without LLVM (fallback-only mode):
//   cmake ...  (omit DDUCKDB_JIT_ENABLED)
//   CanCompile() always returns false → interpreter is used everywhere.
//===----------------------------------------------------------------------===//

#include "duckdb/execution/jit/jit_compiler.hpp"
#include "duckdb/planner/expression/list.hpp"

// ============================================================================
// LLVM JIT path — only compiled when -DDUCKDB_JIT_ENABLED=ON
// ============================================================================
#ifdef DUCKDB_JIT_ENABLED

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>

#include <memory>
#include <string>
#include <vector>

// LLVM version compat: function address retrieval changed in LLVM 17.
#if LLVM_VERSION_MAJOR >= 17
#  define JIT_GET_FN_PTR(sym, FnType) (sym).getAddress().toPtr<FnType>()
#else
#  define JIT_GET_FN_PTR(sym, FnType) reinterpret_cast<FnType>(static_cast<uintptr_t>((sym).getAddress()))
#endif

// LLVM version compat: opaque pointers became default in LLVM 17.
// We use PointerType::getUnqual(elem_ty) everywhere; in LLVM 17+ that returns
// an opaque `ptr` and the elem_ty argument is ignored.  BitCast between
// pointer types becomes a no-op in opaque-pointer mode — correct behaviour.

namespace duckdb {

// ── Type helpers ─────────────────────────────────────────────────────────────

/// Map a DuckDB PhysicalType to the corresponding LLVM type.
/// Returns nullptr for unsupported types.
static llvm::Type *PhysTypeToLLVM(PhysicalType pt, llvm::LLVMContext &ctx) {
	switch (pt) {
	case PhysicalType::BOOL:   return llvm::Type::getInt8Ty(ctx);   // stored as 1-byte int
	case PhysicalType::INT8:   return llvm::Type::getInt8Ty(ctx);
	case PhysicalType::INT16:  return llvm::Type::getInt16Ty(ctx);
	case PhysicalType::INT32:  return llvm::Type::getInt32Ty(ctx);
	case PhysicalType::INT64:  return llvm::Type::getInt64Ty(ctx);
	case PhysicalType::FLOAT:  return llvm::Type::getFloatTy(ctx);
	case PhysicalType::DOUBLE: return llvm::Type::getDoubleTy(ctx);
	default:                   return nullptr;
	}
}

/// True for the numeric physical types we can JIT-compile.
static bool IsSupportedPhysType(PhysicalType pt) {
	return PhysTypeToLLVM(pt, *reinterpret_cast<llvm::LLVMContext *>(1)) != nullptr
	       // ↑ quick structural check — we check via the map rather than listing twice
	       || false; // suppress "unused" warnings on older compilers
	// Simpler version:
	switch (pt) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return true;
	default:
		return false;
	}
}

static bool IsFloatingPt(PhysicalType pt) {
	return pt == PhysicalType::FLOAT || pt == PhysicalType::DOUBLE;
}

/// Numeric rank used for type promotion: higher rank = wider type.
static int TypeRank(PhysicalType pt) {
	switch (pt) {
	case PhysicalType::BOOL:   return 0;
	case PhysicalType::INT8:   return 1;
	case PhysicalType::INT16:  return 2;
	case PhysicalType::INT32:  return 3;
	case PhysicalType::INT64:  return 4;
	case PhysicalType::FLOAT:  return 5;
	case PhysicalType::DOUBLE: return 6;
	default:                   return -1;
	}
}

/// Return the "wider" of two physical types (standard numeric promotion).
static PhysicalType CommonType(PhysicalType a, PhysicalType b) {
	return TypeRank(a) >= TypeRank(b) ? a : b;
}

/// Emit IR to promote/convert `val` from physical type `from` to `to`.
/// Handles: integer widening (SExt), integer truncation (Trunc),
///          int→float (SIToFP), float widening (FPExt), float narrowing (FPTrunc),
///          float→int (FPToSI).
static llvm::Value *PromoteValue(llvm::Value *val, PhysicalType from, PhysicalType to,
                                 llvm::IRBuilder<> &builder, llvm::LLVMContext &ctx) {
	if (from == to) {
		return val;
	}
	llvm::Type *to_ty = PhysTypeToLLVM(to, ctx);
	bool from_fp = IsFloatingPt(from);
	bool to_fp = IsFloatingPt(to);

	if (from_fp && to_fp) {
		return TypeRank(to) > TypeRank(from) ? builder.CreateFPExt(val, to_ty, "fpext")
		                                     : builder.CreateFPTrunc(val, to_ty, "fptrunc");
	}
	if (!from_fp && to_fp) {
		return builder.CreateSIToFP(val, to_ty, "sitofp");
	}
	if (from_fp && !to_fp) {
		return builder.CreateFPToSI(val, to_ty, "fptosi");
	}
	// Both integer:
	return TypeRank(to) > TypeRank(from) ? builder.CreateSExt(val, to_ty, "sext")
	                                     : builder.CreateTrunc(val, to_ty, "trunc");
}

// ── CanCompile (recursive tree check) ────────────────────────────────────────

static bool CanCompileRec(const Expression &expr) {
	// Check the return type of this node.
	if (!IsSupportedPhysType(expr.return_type.InternalType())) {
		return false;
	}

	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF:
	case ExpressionClass::BOUND_CONSTANT:
		return true;

	case ExpressionClass::BOUND_FUNCTION: {
		auto &fn = expr.Cast<BoundFunctionExpression>();
		// Only binary arithmetic for now.
		if (fn.children.size() != 2) {
			return false;
		}
		const auto &name = fn.function.name;
		bool is_arithmetic = (name == "+" || name == "-" || name == "*" || name == "/" ||
		                      name == "add" || name == "subtract" ||
		                      name == "multiply" || name == "divide");
		if (!is_arithmetic) {
			return false;
		}
		for (auto &child : fn.children) {
			if (!CanCompileRec(*child)) {
				return false;
			}
		}
		return true;
	}

	case ExpressionClass::BOUND_COMPARISON: {
		auto &cmp = expr.Cast<BoundComparisonExpression>();
		return CanCompileRec(*cmp.left) && CanCompileRec(*cmp.right);
	}

	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		for (auto &child : conj.children) {
			if (!CanCompileRec(*child)) {
				return false;
			}
		}
		return true;
	}

	case ExpressionClass::BOUND_CAST: {
		auto &cast_expr = expr.Cast<BoundCastExpression>();
		if (!IsSupportedPhysType(cast_expr.child->return_type.InternalType())) {
			return false;
		}
		return CanCompileRec(*cast_expr.child);
	}

	default:
		return false; // BoundWindow, BoundSubquery, etc. → fallback to interpreter
	}
}

// ── IR codegen ───────────────────────────────────────────────────────────────
//
// Codegen() walks the expression tree recursively.
// Returns: {llvm::Value* result, llvm::Value* is_null_flag (i1)}
// is_null = 1 (true) means the value for the current row should be NULL.
// NULL propagation: any NULL input produces a NULL output (simple SQL rule).
// NOTE: AND/OR have three-valued logic in full SQL (NULL AND FALSE = FALSE),
// but for simplicity we use two-valued propagation here. Extend if needed.

struct CodegenCtx {
	llvm::LLVMContext &ctx;
	llvm::IRBuilder<> &builder;
	llvm::Value *loop_idx;      //!< i64 — current row index
	llvm::Value *col_data_arg;  //!< void** — function argument
	llvm::Value *col_valid_arg; //!< uint8_t** — function argument
};

static std::pair<llvm::Value *, llvm::Value *> Codegen(const Expression &expr, CodegenCtx &cc);

/// Helper: load col_data[col_idx] as T*, then load element at loop_idx.
/// Also loads col_valid[col_idx][loop_idx] and converts to i1 is_null flag.
static std::pair<llvm::Value *, llvm::Value *> CodegenColRef(idx_t col_idx, PhysicalType pt,
                                                              CodegenCtx &cc) {
	llvm::LLVMContext &ctx = cc.ctx;
	llvm::IRBuilder<> &B = cc.builder;
	llvm::Type *i8_ty    = llvm::Type::getInt8Ty(ctx);
	llvm::Type *i64_ty   = llvm::Type::getInt64Ty(ctx);
	llvm::Type *elem_ty  = PhysTypeToLLVM(pt, ctx);
	llvm::Type *ptr_ty   = llvm::PointerType::getUnqual(elem_ty);
	llvm::Type *i8ptr_ty = llvm::PointerType::getUnqual(i8_ty);

	llvm::Value *col_idx_val = llvm::ConstantInt::get(i64_ty, col_idx);

	// ── Load col_data[col_idx] → typed pointer ────────────────────────────
	// col_data_arg has type i8** (array of void* / i8* slots).
	// Slot address: GEP with element type = i8* (each slot is one pointer).
	llvm::Value *slot_addr = B.CreateInBoundsGEP(i8ptr_ty, cc.col_data_arg, col_idx_val, "slot_addr");
	llvm::Value *raw_ptr   = B.CreateLoad(i8ptr_ty, slot_addr, "raw_ptr");
	// Reinterpret the i8* as T* so we can index into it.
	llvm::Value *typed_ptr = B.CreateBitCast(raw_ptr, ptr_ty, "typed_ptr");
	// Load value: ((T*)col_data[col_idx])[loop_idx]
	llvm::Value *elem_addr = B.CreateInBoundsGEP(elem_ty, typed_ptr, cc.loop_idx, "elem_addr");
	llvm::Value *val       = B.CreateLoad(elem_ty, elem_addr, "col_val");

	// ── Load col_valid[col_idx][loop_idx] ─────────────────────────────────
	llvm::Value *valid_slot  = B.CreateInBoundsGEP(i8ptr_ty, cc.col_valid_arg, col_idx_val, "valid_slot");
	llvm::Value *valid_arr   = B.CreateLoad(i8ptr_ty, valid_slot, "valid_arr");
	llvm::Value *valid_byte_addr = B.CreateInBoundsGEP(i8_ty, valid_arr, cc.loop_idx, "valid_byte_addr");
	llvm::Value *valid_byte  = B.CreateLoad(i8_ty, valid_byte_addr, "valid_byte");
	// is_null = (valid_byte == 0)
	llvm::Value *is_null = B.CreateICmpEQ(valid_byte,
	                                      llvm::ConstantInt::get(i8_ty, 0), "is_null");
	return {val, is_null};
}

static std::pair<llvm::Value *, llvm::Value *> Codegen(const Expression &expr, CodegenCtx &cc) {
	llvm::LLVMContext &ctx = cc.ctx;
	llvm::IRBuilder<> &B   = cc.builder;
	auto *i1_false = llvm::ConstantInt::getFalse(ctx);
	auto *i1_true  = llvm::ConstantInt::getTrue(ctx);

	switch (expr.GetExpressionClass()) {

	// ── Column reference: load from input array ────────────────────────────
	case ExpressionClass::BOUND_REF: {
		auto &ref = expr.Cast<BoundReferenceExpression>();
		return CodegenColRef(static_cast<idx_t>(ref.index), ref.return_type.InternalType(), cc);
	}

	// ── Constant: emit inline literal ─────────────────────────────────────
	case ExpressionClass::BOUND_CONSTANT: {
		auto &cnst = expr.Cast<BoundConstantExpression>();
		if (cnst.value.IsNull()) {
			// NULL constant → zero value + is_null=true
			llvm::Type *ty = PhysTypeToLLVM(cnst.return_type.InternalType(), ctx);
			return {llvm::Constant::getNullValue(ty), i1_true};
		}
		PhysicalType pt = cnst.return_type.InternalType();
		llvm::Type *ty  = PhysTypeToLLVM(pt, ctx);
		llvm::Value *val = nullptr;
		switch (pt) {
		case PhysicalType::BOOL:
		case PhysicalType::INT8:
			val = llvm::ConstantInt::get(ty, (uint64_t)cnst.value.GetValue<int8_t>(), true);
			break;
		case PhysicalType::INT16:
			val = llvm::ConstantInt::get(ty, (uint64_t)cnst.value.GetValue<int16_t>(), true);
			break;
		case PhysicalType::INT32:
			val = llvm::ConstantInt::get(ty, (uint64_t)cnst.value.GetValue<int32_t>(), true);
			break;
		case PhysicalType::INT64:
			val = llvm::ConstantInt::get(ty, (uint64_t)cnst.value.GetValue<int64_t>(), true);
			break;
		case PhysicalType::FLOAT:
			val = llvm::ConstantFP::get(ty, (double)cnst.value.GetValue<float>());
			break;
		case PhysicalType::DOUBLE:
			val = llvm::ConstantFP::get(ty, cnst.value.GetValue<double>());
			break;
		default:
			val = llvm::Constant::getNullValue(ty);
			break;
		}
		return {val, i1_false};
	}

	// ── Binary arithmetic: +, -, *, / ─────────────────────────────────────
	case ExpressionClass::BOUND_FUNCTION: {
		auto &fn = expr.Cast<BoundFunctionExpression>();
		D_ASSERT(fn.children.size() == 2);

		auto [lhs, lnull] = Codegen(*fn.children[0], cc);
		auto [rhs, rnull] = Codegen(*fn.children[1], cc);
		// NULL propagation: either input null → output null
		llvm::Value *either_null = B.CreateOr(lnull, rnull, "either_null");

		// ── Type promotion: compute in the widest of lhs/rhs/result types ──
		PhysicalType lpt = fn.children[0]->return_type.InternalType();
		PhysicalType rpt = fn.children[1]->return_type.InternalType();
		PhysicalType ret_pt = fn.return_type.InternalType();
		PhysicalType common = CommonType(CommonType(lpt, rpt), ret_pt);

		lhs = PromoteValue(lhs, lpt, common, B, ctx);
		rhs = PromoteValue(rhs, rpt, common, B, ctx);

		const auto &name = fn.function.name;
		bool fp = IsFloatingPt(common);
		llvm::Value *result = nullptr;

		if (name == "+" || name == "add") {
			result = fp ? B.CreateFAdd(lhs, rhs, "fadd") : B.CreateAdd(lhs, rhs, "add");
		} else if (name == "-" || name == "subtract") {
			result = fp ? B.CreateFSub(lhs, rhs, "fsub") : B.CreateSub(lhs, rhs, "sub");
		} else if (name == "*" || name == "multiply") {
			result = fp ? B.CreateFMul(lhs, rhs, "fmul") : B.CreateMul(lhs, rhs, "mul");
		} else if (name == "/" || name == "divide") {
			// NOTE: integer division by zero is undefined behaviour in LLVM IR.
			// A production JIT would emit a zero-check and branch; for a course
			// project we leave that as a known limitation.
			result = fp ? B.CreateFDiv(lhs, rhs, "fdiv") : B.CreateSDiv(lhs, rhs, "sdiv");
		} else {
			// Unsupported op — emit zero + null (CanCompile should have caught this)
			result = llvm::Constant::getNullValue(PhysTypeToLLVM(common, ctx));
			either_null = i1_true;
		}

		// Narrow result back to the declared return type if needed
		result = PromoteValue(result, common, ret_pt, B, ctx);
		return {result, either_null};
	}

	// ── Comparison: =, <>, <, >, <=, >= ──────────────────────────────────
	case ExpressionClass::BOUND_COMPARISON: {
		auto &cmp = expr.Cast<BoundComparisonExpression>();
		auto [lhs, lnull] = Codegen(*cmp.left, cc);
		auto [rhs, rnull] = Codegen(*cmp.right, cc);
		llvm::Value *either_null = B.CreateOr(lnull, rnull, "either_null");

		// Promote both sides to a common type before comparing
		PhysicalType lpt = cmp.left->return_type.InternalType();
		PhysicalType rpt = cmp.right->return_type.InternalType();
		PhysicalType common = CommonType(lpt, rpt);
		lhs = PromoteValue(lhs, lpt, common, B, ctx);
		rhs = PromoteValue(rhs, rpt, common, B, ctx);
		bool fp = IsFloatingPt(common);

		llvm::Value *cmp_i1 = nullptr;
		switch (cmp.GetExpressionType()) {
		case ExpressionType::COMPARE_EQUAL:
			cmp_i1 = fp ? B.CreateFCmpOEQ(lhs, rhs, "feq") : B.CreateICmpEQ(lhs, rhs, "ieq");
			break;
		case ExpressionType::COMPARE_NOTEQUAL:
			cmp_i1 = fp ? B.CreateFCmpONE(lhs, rhs, "fne") : B.CreateICmpNE(lhs, rhs, "ine");
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			cmp_i1 = fp ? B.CreateFCmpOLT(lhs, rhs, "flt") : B.CreateICmpSLT(lhs, rhs, "ilt");
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			cmp_i1 = fp ? B.CreateFCmpOGT(lhs, rhs, "fgt") : B.CreateICmpSGT(lhs, rhs, "igt");
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			cmp_i1 = fp ? B.CreateFCmpOLE(lhs, rhs, "fle") : B.CreateICmpSLE(lhs, rhs, "ile");
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			cmp_i1 = fp ? B.CreateFCmpOGE(lhs, rhs, "fge") : B.CreateICmpSGE(lhs, rhs, "ige");
			break;
		default:
			cmp_i1 = i1_false;
			either_null = i1_true;
		}
		// Store boolean as i8 (0 or 1) to match PhysicalType::BOOL storage
		llvm::Value *as_i8 = B.CreateZExt(cmp_i1, llvm::Type::getInt8Ty(ctx), "bool_i8");
		return {as_i8, either_null};
	}

	// ── Boolean conjunction: AND / OR ─────────────────────────────────────
	// Simple null propagation: any NULL input → NULL output.
	// (Full SQL three-valued logic for AND/OR is a known simplification here.)
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		D_ASSERT(!conj.children.empty());
		auto [acc, acc_null] = Codegen(*conj.children[0], cc);
		bool is_and = (conj.GetExpressionType() == ExpressionType::CONJUNCTION_AND);
		for (idx_t i = 1; i < conj.children.size(); i++) {
			auto [next, next_null] = Codegen(*conj.children[i], cc);
			// Accumulate null flags
			llvm::Value *any_null = B.CreateOr(acc_null, next_null, "conj_null");
			// AND/OR the i8 bytes: since values are always 0 or 1, byte AND == bool AND
			acc = is_and ? B.CreateAnd(acc, next, "and_res") : B.CreateOr(acc, next, "or_res");
			acc_null = any_null;
		}
		return {acc, acc_null};
	}

	// ── Numeric cast ──────────────────────────────────────────────────────
	// try_cast semantics (overflow → NULL) are not implemented here; a
	// production version would emit overflow checks. Document as limitation.
	case ExpressionClass::BOUND_CAST: {
		auto &cast_expr = expr.Cast<BoundCastExpression>();
		auto [val, is_null] = Codegen(*cast_expr.child, cc);
		PhysicalType from_pt = cast_expr.child->return_type.InternalType();
		PhysicalType to_pt   = cast_expr.return_type.InternalType();
		llvm::Value *casted  = PromoteValue(val, from_pt, to_pt, B, ctx);
		return {casted, is_null};
	}

	default:
		// Should never reach here if CanCompile() was checked first.
		{
			llvm::Type *ty = PhysTypeToLLVM(expr.return_type.InternalType(), ctx);
			return {llvm::Constant::getNullValue(ty), i1_true};
		}
	}
}

// ── PIMPL struct: owns the LLJIT instance ────────────────────────────────────

struct JITCompiler::Impl {
	std::unique_ptr<llvm::orc::LLJIT> jit;
	std::string last_ir;
	int counter = 0;

	Impl() {
		llvm::InitializeNativeTarget();
		llvm::InitializeNativeTargetAsmPrinter();
		auto builder_result = llvm::orc::LLJITBuilder().create();
		if (!builder_result) {
			// If LLJIT creation fails, jit stays null; Compile() will return nullptr.
			llvm::consumeError(builder_result.takeError());
			return;
		}
		jit = std::move(*builder_result);
		// Allow JIT code to call into symbols in the host process (needed if
		// the compiler emits calls to runtime helpers, e.g. for division on
		// some architectures).
		jit->getMainJITDylib().addGenerator(
		    llvm::cantFail(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
		        jit->getDataLayout().getGlobalPrefix())));
	}

	JITCompiledFn Compile(const Expression &expr) {
		if (!jit) {
			return nullptr;
		}

		// Each compilation gets a fresh context + module (required by ORC ThreadSafeModule).
		auto ctx = std::make_unique<llvm::LLVMContext>();
		std::string fn_name = "jit_expr_" + std::to_string(counter++);
		auto mod = std::make_unique<llvm::Module>(fn_name, *ctx);

		// ── Build function type ────────────────────────────────────────────
		// void fn(void** col_data, uint8_t** col_valid,
		//         void* result_data, uint8_t* result_valid, uint64_t count)
		llvm::Type *void_ty   = llvm::Type::getVoidTy(*ctx);
		llvm::Type *i8_ty     = llvm::Type::getInt8Ty(*ctx);
		llvm::Type *i8ptr_ty  = llvm::PointerType::getUnqual(i8_ty);   // i8*  / ptr
		llvm::Type *i8pptr_ty = llvm::PointerType::getUnqual(i8ptr_ty); // i8** / ptr
		llvm::Type *i64_ty    = llvm::Type::getInt64Ty(*ctx);

		std::vector<llvm::Type *> params = {
		    i8pptr_ty, // col_data:     void**
		    i8pptr_ty, // col_valid:    uint8_t**
		    i8ptr_ty,  // result_data:  void*
		    i8ptr_ty,  // result_valid: uint8_t*
		    i64_ty     // count:        uint64_t
		};
		auto *fn_ty = llvm::FunctionType::get(void_ty, params, /*isVarArg=*/false);
		auto *fn    = llvm::Function::Create(fn_ty, llvm::Function::ExternalLinkage,
		                                     fn_name, mod.get());

		// Name the arguments for readable IR.
		auto arg = fn->arg_begin();
		llvm::Value *col_data_arg   = &*(arg++); col_data_arg->setName("col_data");
		llvm::Value *col_valid_arg  = &*(arg++); col_valid_arg->setName("col_valid");
		llvm::Value *result_data    = &*(arg++); result_data->setName("result_data");
		llvm::Value *result_valid   = &*(arg++); result_valid->setName("result_valid");
		llvm::Value *count          = &*(arg++); count->setName("count");

		// ── Basic block structure ─────────────────────────────────────────
		// entry → loop_header → loop_body → loop_header → ... → exit
		auto *entry_bb  = llvm::BasicBlock::Create(*ctx, "entry",       fn);
		auto *header_bb = llvm::BasicBlock::Create(*ctx, "loop_header", fn);
		auto *body_bb   = llvm::BasicBlock::Create(*ctx, "loop_body",   fn);
		auto *exit_bb   = llvm::BasicBlock::Create(*ctx, "exit",        fn);

		llvm::IRBuilder<> builder(entry_bb);
		builder.CreateBr(header_bb);

		// ── Loop header: induction variable φ node ────────────────────────
		builder.SetInsertPoint(header_bb);
		auto *i_phi = builder.CreatePHI(i64_ty, 2, "i");
		i_phi->addIncoming(llvm::ConstantInt::get(i64_ty, 0), entry_bb);
		auto *cond = builder.CreateICmpULT(i_phi, count, "loop_cond");
		builder.CreateCondBr(cond, body_bb, exit_bb);

		// ── Loop body: generate expression for current row ────────────────
		builder.SetInsertPoint(body_bb);
		CodegenCtx cc{*ctx, builder, i_phi, col_data_arg, col_valid_arg};
		auto [val, is_null] = Codegen(expr, cc);

		// Store validity byte: 1 if not null, 0 if null.
		llvm::Value *valid_gep  = builder.CreateInBoundsGEP(i8_ty, result_valid, i_phi, "valid_gep");
		llvm::Value *null_i8    = builder.CreateZExt(is_null, i8_ty, "null_i8");
		llvm::Value *valid_i8   = builder.CreateXor(null_i8,
		                             llvm::ConstantInt::get(i8_ty, 1), "valid_i8");
		builder.CreateStore(valid_i8, valid_gep);

		// Store result value (zero for null rows to avoid reading uninitialised memory later).
		PhysicalType res_pt    = expr.return_type.InternalType();
		llvm::Type *res_elem_ty = PhysTypeToLLVM(res_pt, *ctx);
		llvm::Value *res_ptr   = builder.CreateBitCast(result_data,
		                            llvm::PointerType::getUnqual(res_elem_ty), "res_ptr");
		llvm::Value *res_gep   = builder.CreateInBoundsGEP(res_elem_ty, res_ptr, i_phi, "res_gep");
		llvm::Value *zero      = llvm::Constant::getNullValue(res_elem_ty);
		llvm::Value *safe_val  = builder.CreateSelect(is_null, zero, val, "safe_val");
		builder.CreateStore(safe_val, res_gep);

		// Increment i and back-edge to header.
		llvm::Value *i_next = builder.CreateAdd(i_phi,
		                         llvm::ConstantInt::get(i64_ty, 1), "i_next");
		i_phi->addIncoming(i_next, body_bb);
		builder.CreateBr(header_bb);

		// ── Exit ─────────────────────────────────────────────────────────
		builder.SetInsertPoint(exit_bb);
		builder.CreateRetVoid();

		// ── Verify the IR ─────────────────────────────────────────────────
		{
			std::string verify_err;
			llvm::raw_string_ostream voss(verify_err);
			if (llvm::verifyFunction(*fn, &voss)) {
				// IR is malformed — should not happen if CanCompile() passed.
				return nullptr;
			}
		}

		// ── Capture IR string for debugging ──────────────────────────────
		{
			last_ir.clear();
			llvm::raw_string_ostream oss(last_ir);
			mod->print(oss, /*AAW=*/nullptr);
		}

		// ── JIT compile via ORC/LLJIT ────────────────────────────────────
		llvm::orc::ThreadSafeModule tsm(std::move(mod), std::move(ctx));
		if (auto err = jit->addIRModule(std::move(tsm))) {
			llvm::consumeError(std::move(err));
			return nullptr;
		}

		auto sym = jit->lookup(fn_name);
		if (!sym) {
			llvm::consumeError(sym.takeError());
			return nullptr;
		}

		return JIT_GET_FN_PTR(*sym, JITCompiledFn);
	}
};

// ── JITCompiler public API ────────────────────────────────────────────────────

JITCompiler &JITCompiler::GetInstance() {
	static JITCompiler instance;
	return instance;
}
JITCompiler::JITCompiler() : impl(new Impl()) {
}
JITCompiler::~JITCompiler() {
	delete impl;
}

bool JITCompiler::CanCompile(const Expression &expr) const {
	return CanCompileRec(expr);
}

JITCompiledFn JITCompiler::Compile(const Expression &expr) {
	if (!CanCompile(expr)) {
		return nullptr;
	}
	try {
		return impl->Compile(expr);
	} catch (...) {
		return nullptr;
	}
}

void JITCompiler::DumpLastIR() const {
	llvm::errs() << impl->last_ir;
}

const std::string &JITCompiler::GetLastIR() const {
	return impl->last_ir;
}

} // namespace duckdb

// ============================================================================
// FALLBACK path — LLVM not available
// ============================================================================
#else // DUCKDB_JIT_ENABLED not defined

namespace duckdb {

struct JITCompiler::Impl {};

JITCompiler &JITCompiler::GetInstance() {
	static JITCompiler instance;
	return instance;
}
JITCompiler::JITCompiler() : impl(new Impl()) {
}
JITCompiler::~JITCompiler() {
	delete impl;
}
bool JITCompiler::CanCompile(const Expression &) const {
	return false;
}
JITCompiledFn JITCompiler::Compile(const Expression &) {
	return nullptr;
}
void JITCompiler::DumpLastIR() const {
}
const std::string &JITCompiler::GetLastIR() const {
	static const std::string empty;
	return empty;
}

} // namespace duckdb

#endif // DUCKDB_JIT_ENABLED
