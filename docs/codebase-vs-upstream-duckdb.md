# CSCI 543 — Additions vs upstream DuckDB

This document summarizes **what exists in this repository that is not part of stock (upstream) DuckDB**, and **how complete each piece is**. It is meant for another engineer or agent to orient themselves **without reading the C++**.

The embedded tree under `duckdb/` is a **fork or vendor copy of DuckDB** with **local patches**. Upstream DuckDB does **not** ship the JIT profiler, cache, dispatcher, or LLVM expression compiler described below.

Project overview and benchmark table: **[`../README.md`](../README.md)**. Related docs live under **`docs/`** with consistent names (`architecture.md`, `runtime-implementation.md`, etc.).

---

## 1. High-level picture

| Area | Upstream DuckDB | This codebase |
|------|-----------------|---------------|
| Expression execution | `ExpressionExecutor` + vectorized primitives only | Same core, plus **`JITProfiler::Record`** on dense **`Execute`** and filter **`Select`** paths; optional LLVM **`JITCompiler`**; **`JITDispatcher`** available (wire **`TryExecuteJIT`** after **`Record`** for end-to-end JIT) |
| Query planner / optimizer | Full | **Unchanged** for JIT work; no planner hooks for this project |
| LLVM / native expr JIT | None | **`JITCompiler`** (optional build: `DUCKDB_JIT_ENABLED`) |
| Hotness / fingerprints | None | **`JITProfiler`** |
| Compiled-fn cache | None | **`JITCache`** (LRU, fingerprint-keyed) |
| Dispatch JIT vs interpreter | N/A | **`JITDispatcher`** — call **`TryExecuteJIT`** from **`ExpressionExecutor::Execute`** when integrating (not in per-type `execute_*.cpp`) |

---

## 2. New components (files that do not exist upstream)

All live under the DuckDB tree unless noted.

### 2.1 JIT profiler (Part A–style)

- **Headers:** `duckdb/src/include/duckdb/execution/jit/jit_profiler.hpp`  
- **Implementation:** `duckdb/src/execution/jit/jit_profiler.cpp`  
- **Thin include:** `duckdb/src/execution/jit/jit_profiler.hpp` (wrapper path for sources)

**Behavior (conceptual):**

- **`Fingerprint(expr)`** — 64-bit structural hash of the **bound** expression tree (types, shape, comparison ops, function names for `BoundFunctionExpression`, etc.). Constants’ **values** are intentionally **not** fully distinguished (same type → same constant node fingerprint); column refs include binding/index so different slots differ.
- **`Record(expr, tuple_count)`** — thread-safe counters keyed by fingerprint. **Current policy:** only roots passing **`ProfilesAsArithmeticRoot`** (bound `+`, `-`, `*`, `/`, `//`, `%`) increment counts. Filter **`Select`** still invokes **`Record`** on comparisons/BETWEEN; those calls usually no-op until **`Record`** eligibility is extended (e.g. with **`JITCompiler::CanCompile`**).
- **Extras:** optional per-query attribution (`PushQueryScope` / `PopQueryScope`), hotness threshold, callbacks, `ExportSnapshot`, `PrintStats`, overhead atomics.

**Status:** **Implemented and used** from the expression executor (see **§3**).

### 2.2 LLVM expression compiler (Part B–style)

- **Headers:** `duckdb/src/include/duckdb/execution/jit/jit_compiler.hpp`  
- **Implementation:** `duckdb/src/execution/jit/jit_compiler.cpp`

**Behavior:**

- **`CanCompile(expr)`** — structural check for supported bound expression classes and numeric-ish physical types (always available).
- **`Compile(expr)`** — if built with **`-DDUCKDB_JIT_ENABLED=ON`** and LLVM linked: emits a looped native function (`JITCompiledFn`) over flat column pointers and per-row validity bytes; otherwise returns **`nullptr`**.

**Status:** **Implemented** with unit tests (`test_jit_compiler.cpp`). **`TryCompile`** in **`jit_dispatcher.cpp`** may still be a **stub** until it calls **`Compile`** and **`JITCache::Insert`**.

### 2.3 Code cache

- **Headers:** `duckdb/src/include/duckdb/execution/jit/jit_cache.hpp`  
- **Implementation:** `duckdb/src/execution/jit/jit_cache.cpp`

**Behavior:** Thread-safe LRU map **fingerprint →** `std::function`-shaped **`JITCompiledFunction`** (`void(Vector**, Vector&, idx_t)`), plus stats and eviction.

**Status:** **Implemented and tested**; populated when **`TryCompile`** inserts after real compilation.

### 2.4 Dispatcher

- **Headers:** `duckdb/src/include/duckdb/execution/jit/jit_dispatcher.hpp`  
- **Implementation:** `duckdb/src/execution/jit/jit_dispatcher.cpp`

**Behavior (intended):** Cache lookup → optional compile when profiler count ≥ threshold → run compiled wrapper or fall back.

**Status:** API complete; **`TryExecuteJIT`** must be invoked from the executor to affect queries. **`TryCompile`** may be stubbed (log + failure stats). Runtime JIT off until **`SetEnableJIT(true)`**.

### 2.5 Benchmark harness (C++)

- **Headers:** `duckdb/src/include/duckdb/execution/jit/jit_benchmark.hpp`  
- **Implementation:** `duckdb/src/execution/jit/jit_benchmark.cpp`

**Behavior:** Toggles dispatcher/profiler/cache, times queries, optional result comparison, CSV export.

**Status:** **Implemented**; meaningful JIT vs interpreter timing requires LLVM build + **`SetEnableJIT(true)`** + hotness above threshold.

### 2.6 Standalone tools

- `duckdb/tools/jit_profiler_demo.cpp` → executable **`jit_profiler_demo`** (CMake)  
- `duckdb/tools/jit_benchmark_demo.cpp` → executable **`jit_benchmark_demo`**

**Status:** Build targets exist; **`jit_benchmark_demo`** can run end-to-end comparisons when JIT and LLVM are enabled.

### 2.7 Tests

Under `duckdb/test/api/`:

| File | Tags / focus |
|------|----------------|
| `test_jit_profiler.cpp` | `[jit_profiler]` — fingerprints, `Record`, hotness, concurrency |
| `test_jit_cache.cpp` | `[jit_cache]` — LRU, insert/lookup, concurrency |
| `test_jit_dispatcher.cpp` | `[jit_dispatcher]` — config/stats; **does not** run `TryExecuteJIT` with a real executor |
| `test_jit_compiler.cpp` | `[jit_compiler]` — `CanCompile` + exec/IR tests when LLVM enabled |

Registered in `duckdb/test/api/CMakeLists.txt`.

---

## 3. Modifications to upstream DuckDB sources

These are **edits to files that also exist in vanilla DuckDB**.

| File | Change |
|------|--------|
| `duckdb/src/execution/expression_executor.cpp` | After the `count == 0` guard: **`JITProfiler::Record(expr, count)`**; then existing `switch` dispatch. Comments document optional **`TryExecuteJIT`** insertion point. |
| `duckdb/src/execution/expression_executor/execute_comparison.cpp` | **`Select(BoundComparisonExpression, ...)`**: **`Record`** before child evaluation (usually no-op for comparisons under arithmetic-only **`Record`**). |
| `duckdb/src/execution/expression_executor/execute_between.cpp` | **`Select(BoundBetweenExpression, ...)`**: same **`Record`** pattern. |
| `duckdb/src/execution/CMakeLists.txt` | Adds `jit/*.cpp` objects; **`DUCKDB_JIT_ENABLED`** option, LLVM include/link when ON. |
| `duckdb/src/CMakeLists.txt` | Links LLVM libs into `duckdb` / `duckdb_static` when JIT enabled. |
| `duckdb/tools/CMakeLists.txt` | Adds `jit_profiler_demo`, `jit_benchmark_demo`. |

**Design rule:** do not add **`TryExecuteJIT`** inside **`execute_function.cpp`** / **`execute_comparison.cpp`**; use the single **`ExpressionExecutor::Execute`** entry if wiring JIT.

---

## 4. Build / CMake flags

- **Default build:** No LLVM. `JITCompiler::Compile` is a stub returning `nullptr`; `CanCompile` still works.
- **LLVM build:** Configure with **`DUCKDB_JIT_ENABLED=ON`** (see comments in `duckdb/src/execution/CMakeLists.txt`). Requires LLVM development packages and compatible version.

---

## 5. End-to-end status (current tree)

- **Profiler:** **`Record`** is **called** from **`Execute`** and filter **`Select`** paths; **counts** today reflect **arithmetic roots** only unless **`Record`** is extended.
- **Dispatcher / cache:** Implemented; **`TryExecuteJIT`** is useful only when **called** from the executor after **`Record`**. **`TryCompile`** may still be a **stub** (no **`JITCache::Insert`** yet).
- **Compiler:** **`JITCompiler`** works when built with LLVM; connect it from **`TryCompile`** for real speedups.

See **[`jit-source-tour.md`](jit-source-tour.md)** for line-level comments and extension notes.

**JIT remains disabled** until **`JITDispatcher::SetEnableJIT(true)`**.

---

## 6. Documentation index (`docs/`)

| Document | Role |
|----------|------|
| [`architecture.md`](architecture.md) | Diagrams and narrative for the JIT pipeline. |
| [`codebase-vs-upstream-duckdb.md`](codebase-vs-upstream-duckdb.md) | This file — diff vs stock DuckDB. |
| [`runtime-implementation.md`](runtime-implementation.md) | Part C: cache, dispatcher, benchmark API (detail). |
| [`part-c-summary.md`](part-c-summary.md) | Short Part C / runtime summary. |
| [`usage-examples.md`](usage-examples.md) | C++ examples for cache / dispatcher / benchmarks. |
| [`jit-source-tour.md`](jit-source-tour.md) | Maps JIT files to roles and integration points. |

Root **[`README.md`](../README.md)** — project one-pager and **benchmark results** table.

---

## 7. Quick file map (JIT-related)

```
duckdb/src/include/duckdb/execution/jit/
  jit_profiler.hpp, jit_cache.hpp, jit_dispatcher.hpp,
  jit_compiler.hpp, jit_benchmark.hpp

duckdb/src/execution/jit/
  jit_profiler.cpp, jit_cache.cpp, jit_dispatcher.cpp,
  jit_compiler.cpp, jit_benchmark.cpp
  jit_profiler.hpp  (thin wrapper)

duckdb/test/api/
  test_jit_profiler.cpp, test_jit_cache.cpp,
  test_jit_dispatcher.cpp, test_jit_compiler.cpp

duckdb/tools/
  jit_profiler_demo.cpp, jit_benchmark_demo.cpp
```

---

*Update **§3**, **§5**, and **§6** when executor wiring or JIT policy changes.*
