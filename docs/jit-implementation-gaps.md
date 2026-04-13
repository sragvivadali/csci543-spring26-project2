# JIT implementation — gaps and partial work

This list summarizes what is **not fully implemented** or **differs from the project proposal**, based on a code audit of this repository. Use it as a checklist for submission or follow-up work.

---

## Profiler

*(Resolved: single `Record` per batch for function roots; `SetCompilationThreshold` syncs profiler hotness; default **1000** tuples; `Record` counts arithmetic roots **or** `JITCompiler::CanCompile` trees.)*

---

## JIT compiler

- **Floor division (`//`):** Profiler treats `//` as arithmetic; `JITCompiler::CanCompile` does not allow the `//` function name—integer floor division is not compiled.
- **Unary operators:** `CanCompile` requires binary `BoundFunctionExpression` with two children; unary `-` and similar are not compiled.
- **Planner column refs:** `BOUND_COLUMN_REF` is not in the compilable set; execution paths that use `BoundReferenceExpression` are fine.

---

## Dispatcher / flow

- **Profiler increment vs cache miss:** `Record` runs before every `TryExecuteJIT`, including on **cache hits**, so counts are not “only on cache miss.”
- **Filter predicates:** JIT is only attempted for **`BoundFunctionExpression`** in `execute_function.cpp`. Filter **`Select`** paths (e.g. comparisons in `execute_comparison.cpp`) call `Record` but do **not** call `TryExecuteJIT`.

---

## Benchmarking / evaluation (proposal checklist)

- **Two named TPC-DS queries documented** (e.g. arithmetic-heavy with columns like `SS_SALES_PRICE`, `SS_EXT_DISCOUNT_AMT`): not documented in-repo.
- **Microbenchmark** exactly `SELECT a * b + c FROM t WHERE a * 1.1 > 50` at **1M, 10M, 100M** rows: no dedicated runner found; only related SQL in tests/demos.
- **SF 1, 10, 100 matrix:** `tpcds_jit_benchmark` takes one scale factor; no bundled script that runs all three for the JIT experiment.
- **Compilation overhead:** `JITCache::Insert` is always called with `compile_time_us = 0`; compile time is not measured or reported.
- **Threshold sweep 100–10,000:** not automated; examples elsewhere use different ranges (e.g. 1000–50000).
- **Runtime disable JIT:** `JITDispatcher::SetEnableJIT` exists for C++ tools; no documented **SQL/CLI** flag for end users on the standard shell binary.

---

## Threading / stats (minor)

- **Dispatcher statistics** (`jit_executions`, `interpreter_executions`, etc.) are non-atomic counters; possible races under multi-threaded query execution (low priority if workloads are single-threaded).

---

## Build note (LLVM required for JIT benchmarks)

JIT benchmarks (`tpcds_jit_benchmark`, `JITDispatcher::SetEnableJIT(true)`, etc.) only exercise **real** LLVM compilation if the binary is built with **`DUCKDB_JIT_ENABLED=ON`** and LLVM is installed (see `duckdb/src/execution/CMakeLists.txt`).

**Configure and build (from the `duckdb` directory):**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DDUCKDB_JIT_ENABLED=ON
# Use -j2 so the compile only runs two jobs at once (easier on a small machine).
cmake --build build --target tpcds_jit_benchmark unittest -j2
```

**Limit DuckDB query threads at benchmark runtime** (separate from compile parallelism): cap how many worker threads DuckDB uses for `dsdgen` and TPC-DS queries:

```bash
export DUCKDB_BENCHMARK_THREADS=2
./build/tools/tpcds_jit_benchmark
```

Unset the variable to keep DuckDB’s default (all cores). The benchmark applies `PRAGMA threads=<value>` on startup when the variable is set.

For the interactive shell or other tools, use `PRAGMA threads=2` or `SET threads TO 2` after connecting.

**Verify:** CMake should log something like `LLVM JIT enabled: LLVM … found`. If LLVM is missing, CMake falls back to `DUCKDB_JIT_ENABLED=OFF`; then `JITCompiler::Compile` is a stub and returns `nullptr`, so the “JIT” run is still interpreter-only—**not** comparable to `SetEnableJIT(false)` vs `true` in a proper LLVM build.

**Baseline vs JIT in a single LLVM-enabled binary:** use `JITDispatcher::SetEnableJIT(false)` for vectorized baseline and `SetEnableJIT(true)` for JIT; that is the intended comparison. A non-LLVM build cannot test that split meaningfully.
