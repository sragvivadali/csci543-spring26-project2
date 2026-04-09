//===----------------------------------------------------------------------===//
// duckdb/execution/jit/jit_compiler.cpp
//
// CSCI 543 — Person B: LLVM JIT Compiler + Type Support + Fallback Logic
//
// IMPORTANT — unity build compatibility:
// DuckDB concatenates .cpp files into ub_duckdb_execution.cpp. ALL #includes
// must appear here at file scope BEFORE namespace duckdb { opens, otherwise
// they land inside the duckdb namespace and LLVM headers cannot find ::llvm
// or std:: symbols.
//===----------------------------------------------------------------------===//

// ── Standard / DuckDB includes (always compiled) ─────────────────────────────
#include "duckdb/execution/jit/jit_compiler.hpp"
#include "duckdb/planner/expression/list.hpp"
#include <string>

// ── LLVM includes (only when JIT is enabled) ─────────────────────────────────
// These MUST stay here at file scope — never inside namespace duckdb {}.
#ifdef DUCKDB_JIT_ENABLED
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>
#include <vector>

// LLVM API for getting a function pointer from a looked-up symbol changed across versions:
//   <17: getAddress() returns uint64_t
//   17-21: returns ExecutorSymbolDef → .getAddress().toPtr<>()
//   22+: lookup() returns ExecutorAddr directly → .toPtr<>()
#if LLVM_VERSION_MAJOR >= 22
#  define JIT_GET_FN_PTR(sym, FnType) (sym).toPtr<FnType>()
#elif LLVM_VERSION_MAJOR >= 17
#  define JIT_GET_FN_PTR(sym, FnType) (sym).getAddress().toPtr<FnType>()
#else
#  define JIT_GET_FN_PTR(sym, FnType) \
     reinterpret_cast<FnType>(static_cast<uintptr_t>((sym).getAddress()))
#endif
#endif // DUCKDB_JIT_ENABLED

// ── Everything below is inside namespace duckdb ───────────────────────────────
namespace duckdb {

// ============================================================================
// Section 1 — Type support + CanCompile
// No LLVM dependency. Compiled in ALL builds so structural expression checks
// work even when LLVM is not linked.
// ============================================================================

static bool IsSupportedPhysType(PhysicalType pt) {
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

static bool CanCompileRec(const Expression &expr) {
	if (!IsSupportedPhysType(expr.return_type.InternalType())) {
		return false;
	}
	switch (expr.GetExpressionClass()) {

	case ExpressionClass::BOUND_REF:
	case ExpressionClass::BOUND_CONSTANT:
		return true;

	case ExpressionClass::BOUND_FUNCTION: {
		auto &fn = expr.Cast<BoundFunctionExpression>();
		if (fn.children.size() != 2) {
			return false;
		}
		const auto &name = fn.function.name;
		bool ok = (name == "+" || name == "-" || name == "*" || name == "/" ||
		           name == "add" || name == "subtract" ||
		           name == "multiply" || name == "divide");
		if (!ok) {
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
		auto &c = expr.Cast<BoundCastExpression>();
		return IsSupportedPhysType(c.child->return_type.InternalType()) &&
		       CanCompileRec(*c.child);
	}

	default:
		return false;
	}
}

// Public API — shared by both builds (no LLVM needed).
bool JITCompiler::CanCompile(const Expression &expr) const {
	return CanCompileRec(expr);
}

// ============================================================================
// Section 2 — LLVM codegen (only when -DDUCKDB_JIT_ENABLED=ON)
// ============================================================================
#ifdef DUCKDB_JIT_ENABLED

// ── LLVM type helpers ─────────────────────────────────────────────────────────

static llvm::Type *PhysToLLVM(PhysicalType pt, llvm::LLVMContext &ctx) {
	switch (pt) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:   return llvm::Type::getInt8Ty(ctx);
	case PhysicalType::INT16:  return llvm::Type::getInt16Ty(ctx);
	case PhysicalType::INT32:  return llvm::Type::getInt32Ty(ctx);
	case PhysicalType::INT64:  return llvm::Type::getInt64Ty(ctx);
	case PhysicalType::FLOAT:  return llvm::Type::getFloatTy(ctx);
	case PhysicalType::DOUBLE: return llvm::Type::getDoubleTy(ctx);
	default:                   return nullptr;
	}
}

static bool IsFP(PhysicalType pt) {
	return pt == PhysicalType::FLOAT || pt == PhysicalType::DOUBLE;
}

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

static PhysicalType CommonType(PhysicalType a, PhysicalType b) {
	return TypeRank(a) >= TypeRank(b) ? a : b;
}

static llvm::Value *Promote(llvm::Value *v, PhysicalType from, PhysicalType to,
                            llvm::IRBuilder<> &B, llvm::LLVMContext &ctx) {
	if (from == to) {
		return v;
	}
	llvm::Type *dst = PhysToLLVM(to, ctx);
	bool ffp = IsFP(from), tfp = IsFP(to);
	if (ffp && tfp)  { return TypeRank(to) > TypeRank(from) ? B.CreateFPExt(v, dst, "fpext")   : B.CreateFPTrunc(v, dst, "fptrunc"); }
	if (!ffp && tfp) { return B.CreateSIToFP(v, dst, "sitofp"); }
	if (ffp && !tfp) { return B.CreateFPToSI(v, dst, "fptosi"); }
	return TypeRank(to) > TypeRank(from) ? B.CreateSExt(v, dst, "sext") : B.CreateTrunc(v, dst, "trunc");
}

// ── IR codegen ────────────────────────────────────────────────────────────────
// Returns {value, is_null (i1)}. NULL propagation: any NULL input → NULL out.

struct CCtx {
	llvm::LLVMContext &ctx;
	llvm::IRBuilder<> &B;
	llvm::Value *idx;       // i64 loop induction variable
	llvm::Value *col_data;  // void**     (col_data[i] → flat value array)
	llvm::Value *col_valid; // uint8_t**  (col_valid[i] → validity byte array)
};

static std::pair<llvm::Value *, llvm::Value *> Codegen(const Expression &, CCtx &);

static std::pair<llvm::Value *, llvm::Value *> CodegenRef(idx_t col, PhysicalType pt, CCtx &cc) {
	auto &ctx   = cc.ctx;
	auto &B     = cc.B;
	llvm::Type *i8   = llvm::Type::getInt8Ty(ctx);
	llvm::Type *i64  = llvm::Type::getInt64Ty(ctx);
	llvm::Type *elem = PhysToLLVM(pt, ctx);
	llvm::Type *eptr = llvm::PointerType::getUnqual(elem);
	llvm::Type *bptr = llvm::PointerType::getUnqual(i8);   // i8* slot type

	llvm::Value *ci = llvm::ConstantInt::get(i64, col);

	// Load col_data[col] → typed pointer → element
	auto *dslot = B.CreateInBoundsGEP(bptr, cc.col_data, ci, "dslot");
	auto *draw  = B.CreateLoad(bptr, dslot, "draw");
	auto *dtyp  = B.CreateBitCast(draw, eptr, "dtyp");
	auto *val   = B.CreateLoad(elem, B.CreateInBoundsGEP(elem, dtyp, cc.idx, "ep"), "val");

	// Load col_valid[col][idx] → is_null flag
	auto *vslot = B.CreateInBoundsGEP(bptr, cc.col_valid, ci, "vslot");
	auto *varr  = B.CreateLoad(bptr, vslot, "varr");
	auto *vbyte = B.CreateLoad(i8, B.CreateInBoundsGEP(i8, varr, cc.idx, "vbp"), "vbyte");
	auto *isnull = B.CreateICmpEQ(vbyte, llvm::ConstantInt::get(i8, 0), "isnull");
	return {val, isnull};
}

static std::pair<llvm::Value *, llvm::Value *> Codegen(const Expression &expr, CCtx &cc) {
	auto &ctx = cc.ctx;
	auto &B   = cc.B;
	auto *F   = llvm::ConstantInt::getFalse(ctx);
	auto *T   = llvm::ConstantInt::getTrue(ctx);

	switch (expr.GetExpressionClass()) {

	case ExpressionClass::BOUND_REF: {
		auto &r = expr.Cast<BoundReferenceExpression>();
		return CodegenRef((idx_t)r.index, r.return_type.InternalType(), cc);
	}

	case ExpressionClass::BOUND_CONSTANT: {
		auto &c = expr.Cast<BoundConstantExpression>();
		llvm::Type *ty = PhysToLLVM(c.return_type.InternalType(), ctx);
		if (c.value.IsNull()) {
			return {llvm::Constant::getNullValue(ty), T};
		}
		PhysicalType pt = c.return_type.InternalType();
		llvm::Value *v = nullptr;
		switch (pt) {
		case PhysicalType::BOOL:
		case PhysicalType::INT8:  v = llvm::ConstantInt::get(ty, (uint64_t)c.value.GetValue<int8_t>(),  true); break;
		case PhysicalType::INT16: v = llvm::ConstantInt::get(ty, (uint64_t)c.value.GetValue<int16_t>(), true); break;
		case PhysicalType::INT32: v = llvm::ConstantInt::get(ty, (uint64_t)c.value.GetValue<int32_t>(), true); break;
		case PhysicalType::INT64: v = llvm::ConstantInt::get(ty, (uint64_t)c.value.GetValue<int64_t>(), true); break;
		case PhysicalType::FLOAT: v = llvm::ConstantFP::get(ty, (double)c.value.GetValue<float>());            break;
		case PhysicalType::DOUBLE:v = llvm::ConstantFP::get(ty, c.value.GetValue<double>());                   break;
		default:                  v = llvm::Constant::getNullValue(ty);                                         break;
		}
		return {v, F};
	}

	case ExpressionClass::BOUND_FUNCTION: {
		auto &fn = expr.Cast<BoundFunctionExpression>();
		auto [lv, ln] = Codegen(*fn.children[0], cc);
		auto [rv, rn] = Codegen(*fn.children[1], cc);
		auto *en = B.CreateOr(ln, rn, "either_null");

		PhysicalType lp = fn.children[0]->return_type.InternalType();
		PhysicalType rp = fn.children[1]->return_type.InternalType();
		PhysicalType rtp = fn.return_type.InternalType();
		PhysicalType com = CommonType(CommonType(lp, rp), rtp);
		lv = Promote(lv, lp, com, B, ctx);
		rv = Promote(rv, rp, com, B, ctx);

		const auto &nm = fn.function.name;
		bool fp = IsFP(com);
		llvm::Value *res = nullptr;
		if      (nm == "+" || nm == "add")      { res = fp ? B.CreateFAdd(lv,rv,"fadd") : B.CreateAdd(lv,rv,"add"); }
		else if (nm == "-" || nm == "subtract")  { res = fp ? B.CreateFSub(lv,rv,"fsub") : B.CreateSub(lv,rv,"sub"); }
		else if (nm == "*" || nm == "multiply")  { res = fp ? B.CreateFMul(lv,rv,"fmul") : B.CreateMul(lv,rv,"mul"); }
		else if (nm == "/" || nm == "divide")    { res = fp ? B.CreateFDiv(lv,rv,"fdiv") : B.CreateSDiv(lv,rv,"sdiv"); }
		else { res = llvm::Constant::getNullValue(PhysToLLVM(com, ctx)); en = T; }

		return {Promote(res, com, rtp, B, ctx), en};
	}

	case ExpressionClass::BOUND_COMPARISON: {
		auto &cmp = expr.Cast<BoundComparisonExpression>();
		auto [lv, ln] = Codegen(*cmp.left, cc);
		auto [rv, rn] = Codegen(*cmp.right, cc);
		auto *en = B.CreateOr(ln, rn, "either_null");

		PhysicalType lp = cmp.left->return_type.InternalType();
		PhysicalType rp = cmp.right->return_type.InternalType();
		PhysicalType com = CommonType(lp, rp);
		lv = Promote(lv, lp, com, B, ctx);
		rv = Promote(rv, rp, com, B, ctx);
		bool fp = IsFP(com);

		llvm::Value *ci = nullptr;
		switch (cmp.GetExpressionType()) {
		case ExpressionType::COMPARE_EQUAL:               ci = fp ? B.CreateFCmpOEQ(lv,rv) : B.CreateICmpEQ(lv,rv);  break;
		case ExpressionType::COMPARE_NOTEQUAL:            ci = fp ? B.CreateFCmpONE(lv,rv) : B.CreateICmpNE(lv,rv);  break;
		case ExpressionType::COMPARE_LESSTHAN:            ci = fp ? B.CreateFCmpOLT(lv,rv) : B.CreateICmpSLT(lv,rv); break;
		case ExpressionType::COMPARE_GREATERTHAN:         ci = fp ? B.CreateFCmpOGT(lv,rv) : B.CreateICmpSGT(lv,rv); break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:   ci = fp ? B.CreateFCmpOLE(lv,rv) : B.CreateICmpSLE(lv,rv); break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:ci = fp ? B.CreateFCmpOGE(lv,rv) : B.CreateICmpSGE(lv,rv); break;
		default: ci = F; en = T;
		}
		return {B.CreateZExt(ci, llvm::Type::getInt8Ty(ctx), "bool_i8"), en};
	}

	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto [acc, an] = Codegen(*conj.children[0], cc);
		bool is_and = (conj.GetExpressionType() == ExpressionType::CONJUNCTION_AND);
		for (idx_t i = 1; i < conj.children.size(); i++) {
			auto [nv, nn] = Codegen(*conj.children[i], cc);
			an  = B.CreateOr(an, nn, "any_null");
			acc = is_and ? B.CreateAnd(acc, nv, "and_res") : B.CreateOr(acc, nv, "or_res");
		}
		return {acc, an};
	}

	case ExpressionClass::BOUND_CAST: {
		auto &ce = expr.Cast<BoundCastExpression>();
		auto [v, n] = Codegen(*ce.child, cc);
		return {Promote(v, ce.child->return_type.InternalType(),
		                ce.return_type.InternalType(), B, ctx), n};
	}

	default: {
		llvm::Type *ty = PhysToLLVM(expr.return_type.InternalType(), ctx);
		return {llvm::Constant::getNullValue(ty), T};
	}
	}
}

// ── PIMPL ────────────────────────────────────────────────────────────────────

struct JITCompiler::Impl {
	std::unique_ptr<llvm::orc::LLJIT> jit;
	std::string last_ir;
	int counter = 0;

	Impl() {
		llvm::InitializeNativeTarget();
		llvm::InitializeNativeTargetAsmPrinter();
		auto r = llvm::orc::LLJITBuilder().create();
		if (!r) { llvm::consumeError(r.takeError()); return; }
		jit = std::move(*r);
		jit->getMainJITDylib().addGenerator(
		    llvm::cantFail(llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
		        jit->getDataLayout().getGlobalPrefix())));
	}

	JITCompiledFn Compile(const Expression &expr) {
		if (!jit) { return nullptr; }

		auto ctx    = std::make_unique<llvm::LLVMContext>();
		std::string fn_name = "jit_expr_" + std::to_string(counter++);
		auto mod    = std::make_unique<llvm::Module>(fn_name, *ctx);

		// void fn(void** col_data, uint8_t** col_valid,
		//         void* result_data, uint8_t* result_valid, uint64_t count)
		llvm::Type *vd   = llvm::Type::getVoidTy(*ctx);
		llvm::Type *i8   = llvm::Type::getInt8Ty(*ctx);
		llvm::Type *bptr = llvm::PointerType::getUnqual(i8);
		llvm::Type *ppbp = llvm::PointerType::getUnqual(bptr);
		llvm::Type *i64  = llvm::Type::getInt64Ty(*ctx);

		auto *fty = llvm::FunctionType::get(vd, {ppbp, ppbp, bptr, bptr, i64}, false);
		auto *fn  = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, fn_name, mod.get());

		auto a = fn->arg_begin();
		llvm::Value *cd = &*(a++); cd->setName("col_data");
		llvm::Value *cv = &*(a++); cv->setName("col_valid");
		llvm::Value *rd = &*(a++); rd->setName("result_data");
		llvm::Value *rv = &*(a++); rv->setName("result_valid");
		llvm::Value *cnt= &*(a++); cnt->setName("count");

		auto *entry  = llvm::BasicBlock::Create(*ctx, "entry",       fn);
		auto *header = llvm::BasicBlock::Create(*ctx, "loop_header", fn);
		auto *body   = llvm::BasicBlock::Create(*ctx, "loop_body",   fn);
		auto *exit   = llvm::BasicBlock::Create(*ctx, "exit",        fn);

		llvm::IRBuilder<> B(entry);
		B.CreateBr(header);

		B.SetInsertPoint(header);
		auto *phi = B.CreatePHI(i64, 2, "i");
		phi->addIncoming(llvm::ConstantInt::get(i64, 0), entry);
		B.CreateCondBr(B.CreateICmpULT(phi, cnt, "cond"), body, exit);

		B.SetInsertPoint(body);
		CCtx cc{*ctx, B, phi, cd, cv};
		auto [val, isnull] = Codegen(expr, cc);

		// Write validity byte
		auto *null_i8  = B.CreateZExt(isnull, i8, "null_i8");
		auto *valid_i8 = B.CreateXor(null_i8, llvm::ConstantInt::get(i8, 1), "valid_i8");
		B.CreateStore(valid_i8, B.CreateInBoundsGEP(i8, rv, phi, "vgep"));

		// Write result value (zero for NULL rows)
		PhysicalType rpt  = expr.return_type.InternalType();
		llvm::Type  *rety = PhysToLLVM(rpt, *ctx);
		auto *rptr = B.CreateBitCast(rd, llvm::PointerType::getUnqual(rety), "rptr");
		auto *safe = B.CreateSelect(isnull, llvm::Constant::getNullValue(rety), val, "safe");
		B.CreateStore(safe, B.CreateInBoundsGEP(rety, rptr, phi, "rgep"));

		auto *nxt = B.CreateAdd(phi, llvm::ConstantInt::get(i64, 1), "nxt");
		phi->addIncoming(nxt, body);
		B.CreateBr(header);

		B.SetInsertPoint(exit);
		B.CreateRetVoid();

		// Verify IR
		{
			std::string err; llvm::raw_string_ostream oss(err);
			if (llvm::verifyFunction(*fn, &oss)) { return nullptr; }
		}

		// Capture IR for debugging
		{ last_ir.clear(); llvm::raw_string_ostream oss(last_ir); mod->print(oss, nullptr); }

		// JIT compile
		llvm::orc::ThreadSafeModule tsm(std::move(mod), std::move(ctx));
		if (auto err = jit->addIRModule(std::move(tsm))) {
			llvm::consumeError(std::move(err)); return nullptr;
		}
		auto sym = jit->lookup(fn_name);
		if (!sym) { llvm::consumeError(sym.takeError()); return nullptr; }
		return JIT_GET_FN_PTR(*sym, JITCompiledFn);
	}
};

// ── Public API (LLVM build) ───────────────────────────────────────────────────

JITCompiler &JITCompiler::GetInstance() { static auto *instance = new JITCompiler(); return *instance; }
JITCompiler::JITCompiler() : impl(new Impl()) {}
JITCompiler::~JITCompiler() { delete impl; }

JITCompiledFn JITCompiler::Compile(const Expression &expr) {
	if (!CanCompile(expr)) { return nullptr; }
	try { return impl->Compile(expr); } catch (...) { return nullptr; }
}
void JITCompiler::DumpLastIR() const { llvm::errs() << impl->last_ir; }
const std::string &JITCompiler::GetLastIR() const { return impl->last_ir; }

// ============================================================================
// Section 3 — Fallback build (no LLVM)
// ============================================================================
#else // !DUCKDB_JIT_ENABLED

struct JITCompiler::Impl {};

JITCompiler &JITCompiler::GetInstance() { static auto *instance = new JITCompiler(); return *instance; }
JITCompiler::JITCompiler() : impl(new Impl()) {}
JITCompiler::~JITCompiler() { delete impl; }
JITCompiledFn JITCompiler::Compile(const Expression &) { return nullptr; }
void JITCompiler::DumpLastIR() const {}
const std::string &JITCompiler::GetLastIR() const { static const auto *e = new std::string(); return *e; }

#endif // DUCKDB_JIT_ENABLED

} // namespace duckdb
