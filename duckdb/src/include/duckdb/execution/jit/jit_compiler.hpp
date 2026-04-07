//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/jit/jit_compiler.hpp
//
//===----------------------------------------------------------------------===//
//
// JITCompiler — LLVM-based expression compiler (CSCI 543 project, Person B)
//
// Responsibility: take a bound DuckDB expression tree and compile it to
// native machine code via LLVM so it can run faster than the interpreter.
//
// Compiled function contract
// --------------------------
// The compiled function has this C signature (JITCompiledFn typedef below):
//
//   void fn(void**    col_data,     // col_data[i]  → flat array of column i values
//           uint8_t** col_valid,    // col_valid[i] → byte array: 1=valid, 0=NULL
//           void*     result_data,  // flat output value array (physical type of return)
//           uint8_t*  result_valid, // output validity: 1=valid, 0=NULL
//           uint64_t  count)        // number of rows to process
//
// Column indices come from BoundReferenceExpression::index, which is the
// column slot in the DataChunk passed by Person C's dispatcher.
//
// Validity convention
// -------------------
// DuckDB uses bitmask validity internally. Person C's dispatcher must convert
// between the internal bitmask (ValidityMask) and the per-byte arrays expected
// here before/after calling the compiled function.
//
// Fallback
// --------
// Call CanCompile(expr) before Compile(). If it returns false (unsupported
// type/op, or LLVM not linked), the dispatcher falls back to the interpreter.
//
// Supported expressions
// ---------------------
//   BoundReferenceExpression    — reads a column slot
//   BoundConstantExpression     — inline constant (numeric only)
//   BoundFunctionExpression     — binary arithmetic: +, -, *, /
//   BoundComparisonExpression   — =, <>, <, >, <=, >=
//   BoundConjunctionExpression  — AND, OR (simple null propagation)
//   BoundCastExpression         — numeric widening/narrowing casts
//
// Supported physical types: BOOL, INT8, INT16, INT32, INT64, FLOAT, DOUBLE
//
// Thread safety
// -------------
// Compile() is NOT thread-safe. Wrap in a mutex if called from multiple
// threads. CanCompile() is pure and safe to call concurrently.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"
#include <cstdint>
#include <string>

namespace duckdb {

//! Signature of every JIT-compiled vectorized expression function.
//! See header comment for full contract.
using JITCompiledFn = void (*)(void **col_data, uint8_t **col_valid,
                               void *result_data, uint8_t *result_valid,
                               uint64_t count);

class JITCompiler {
public:
	//! Returns the process-wide singleton JITCompiler.
	static JITCompiler &GetInstance();

	//! Returns true if the expression tree can be JIT-compiled:
	//!   - all types are numeric (bool/int8/16/32/64/float/double)
	//!   - all expression classes are in the supported set (see header)
	//!   - DUCKDB_JIT_ENABLED was defined at build time
	//! Pure function — safe to call from any thread.
	bool CanCompile(const Expression &expr) const;

	//! Compiles expr to a native JITCompiledFn.
	//! Returns nullptr if compilation fails (call CanCompile first).
	//! The returned pointer is valid for the lifetime of this JITCompiler.
	//! NOT thread-safe; protect with a mutex if needed.
	JITCompiledFn Compile(const Expression &expr);

	//! Dumps LLVM IR of the last compiled expression to stderr.
	//! Useful for debugging and unit tests.
	void DumpLastIR() const;

	//! Returns the LLVM IR string of the last compiled expression.
	const std::string &GetLastIR() const;

private:
	JITCompiler();
	~JITCompiler();
	JITCompiler(const JITCompiler &) = delete;
	JITCompiler &operator=(const JITCompiler &) = delete;

	struct Impl; //!< PIMPL: hides all LLVM headers from callers
	Impl *impl;
};

} // namespace duckdb
