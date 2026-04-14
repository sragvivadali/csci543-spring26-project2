# Source Tour

This page maps the main implementation files and why they matter.

## Runtime Entry Points

- `duckdb/src/execution/expression_executor.cpp`
  - Central execution dispatch.
  - Profiling hook and generic expression handling.

- `duckdb/src/execution/expression_executor/execute_function.cpp`
  - Function expression execution path.
  - JIT fast-path invocation in function evaluation.

- `duckdb/src/execution/expression_executor/execute_comparison.cpp`
  - Comparison execution and filter select path.
  - JIT support for filter predicates.

## JIT Subsystem

- `duckdb/src/execution/jit/jit_profiler.cpp` + header
  - Fingerprinting and hotness counters.

- `duckdb/src/execution/jit/jit_dispatcher.cpp` + header
  - Runtime JIT/interpreter selection.
  - Compile attempts and dispatch statistics.

- `duckdb/src/execution/jit/jit_cache.cpp` + header
  - LRU cache and lookup/insert behavior.

- `duckdb/src/execution/jit/jit_compiler.cpp` + header
  - Structural compilability checks and LLVM IR/native code generation.

## Benchmarks / Tools

- `duckdb/tools/tpch_jit_benchmark.cpp`
- `duckdb/tools/tpcds_jit_benchmark.cpp`
- `duckdb/tools/jit_micro_benchmark.cpp`

## Tests

- `duckdb/test/api/test_jit_profiler.cpp`
- `duckdb/test/api/test_jit_cache.cpp`
- `duckdb/test/api/test_jit_dispatcher.cpp`
- `duckdb/test/api/test_jit_compiler.cpp`
