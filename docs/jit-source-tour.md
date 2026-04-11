# JIT extension — source code tour (CSCI 543)

This guide maps **documentation to files** so you can navigate the modified DuckDB tree quickly.

## Execution flow (intended design)

1. **`ExpressionExecutor::Execute`** — Every dense evaluation of a bound expression passes through here (projections, materialized booleans, etc.). **`JITProfiler::Record(expr, count)`** runs first so hotness matches batch size.
2. **Optional:** **`JITDispatcher::TryExecuteJIT`** — If inserted after `Record` (and e.g. `sel == nullptr`), can run code from **`JITCache`** or trigger **`TryCompile`**. Keep this at the **top-level** executor, not inside `execute_function.cpp` / `execute_comparison.cpp` per-type bodies.
3. **Filter `Select` paths** — **`execute_comparison.cpp`** and **`execute_between.cpp`** call **`Record`** at the start of **`Select(...)`** so predicate batches contribute to fingerprints when eligibility allows.

## File reference

| Area | Path | Role |
|------|------|------|
| Main executor hook | `duckdb/src/execution/expression_executor.cpp` | `Record` after `count == 0` guard; comments describe optional `TryExecuteJIT`. |
| Comparison filter | `duckdb/src/execution/expression_executor/execute_comparison.cpp` | `Select(BoundComparisonExpression)` → `Record` then child `Execute` + `VectorOperations::*`. |
| Between filter | `duckdb/src/execution/expression_executor/execute_between.cpp` | `Select(BoundBetweenExpression)` → `Record` then ternary comparison logic. |
| Profiler | `duckdb/src/execution/jit/jit_profiler.cpp`, `.../jit_profiler.hpp` | FNV fingerprint, **`ProfilesAsArithmeticRoot`**, **`Record`**, hotness / export APIs. |
| Cache | `duckdb/src/execution/jit/jit_cache.cpp`, `.../jit_cache.hpp` | LRU cache keyed by same fingerprint as profiler. |
| Dispatcher | `duckdb/src/execution/jit/jit_dispatcher.cpp`, `.../jit_dispatcher.hpp` | **`TryExecuteJIT`**, **`ShouldCompile`**, **`TryCompile`** (stub until LLVM insert wired). |
| Compiler | `duckdb/src/execution/jit/jit_compiler.cpp`, `.../jit_compiler.hpp` | **`CanCompile`**, **`Compile`** when `DUCKDB_JIT_ENABLED=ON`. |
| Benchmarks | `duckdb/src/execution/jit/jit_benchmark.cpp`, `duckdb/tools/jit_benchmark_demo.cpp` | Timing harness and demo binary. |
| Tests | `duckdb/test/api/test_jit_*.cpp` | Catch2 tags `[jit_profiler]`, `[jit_cache]`, `[jit_dispatcher]`, `[jit_compiler]`. |

## Build flags

- **`DUCKDB_JIT_ENABLED=OFF` (default):** **`Compile`** returns `nullptr`; **`CanCompile`** still usable from tests.
- **`DUCKDB_JIT_ENABLED=ON`:** Needs LLVM (see `duckdb/src/execution/CMakeLists.txt`).

## Extending hotness beyond arithmetic

To have **filter comparisons** affect **`JITDispatcher::ShouldCompile`**, extend **`JITProfiler::Record`** (e.g. also accept roots where **`JITCompiler::CanCompile(expr)`** is true). Keep **`ProfilesAsArithmeticRoot`** as the narrow predicate for API/tests if desired.

## Related docs

- [architecture.md](architecture.md) — diagrams.
- [codebase-vs-upstream-duckdb.md](codebase-vs-upstream-duckdb.md) — diff vs upstream and integration status.
- [runtime-implementation.md](runtime-implementation.md) — Part C runtime detail.
- [usage-examples.md](usage-examples.md) — C++ snippets.
