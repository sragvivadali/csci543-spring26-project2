# CSCI 543 — DuckDB expression JIT (Spring 26)

This repository extends a **vendored DuckDB** tree with a research-oriented **JIT pipeline**: profiling, optional LLVM compilation, an LRU cache, and a runtime dispatcher designed to sit alongside the vectorized interpreter.

## What this project is

- **Goal:** Identify **hot** bound expressions and (when wired) run **compiled** numeric arithmetic / comparisons instead of only the interpreter.
- **Scope:** No planner changes. Instrumentation is in **`ExpressionExecutor`** and filter **`Select`** paths; per-type files (`execute_function.cpp`, etc.) are **not** peppered with dispatch logic.
- **Components:** `JITProfiler` → `JITDispatcher` → `JITCompiler` (optional LLVM) → `JITCache`.

## How we achieved it (current codebase)

| Piece | Role |
|--------|------|
| **JITProfiler** | **`Fingerprint`** + **`Record`**. **`Record`** currently increments counts only for **arithmetic** `BoundFunctionExpression` roots (`+`, `-`, `*`, `/`, `//`, `%`). Filter **`Select`** paths still **call** **`Record`** on comparisons/BETWEEN; those calls are usually a no-op until **`Record`** eligibility is broadened (see comments in `jit_profiler.cpp`). |
| **JITCompiler** | **`CanCompile`** / **`Compile`**: LLVM backend when **`DUCKDB_JIT_ENABLED=ON`**; stub **`Compile`** when off. |
| **JITCache** | Thread-safe LRU, fingerprint-keyed **`JITCompiledFunction`** storage. |
| **JITDispatcher** | **`TryExecuteJIT`**: cache lookup, optional **`TryCompile`** on hot miss, interpreter stats. **`TryCompile`** may still be a **stub** in `jit_dispatcher.cpp` until it calls **`JITCompiler`** and **`JITCache::Insert`**. |
| **Executor** | **`expression_executor.cpp`**: **`Record`** after the `count == 0` guard; **comments** describe where to call **`TryExecuteJIT`** for end-to-end JIT. |
| **Tests & tools** | `duckdb/test/api/test_jit_*.cpp`; `jit_profiler_demo`, `jit_benchmark_demo`. |

**Inline documentation:** CSCI 543-related **file headers** and **local comments** live in the files above (search for `CSCI 543` or read **[docs/jit-source-tour.md](docs/jit-source-tour.md)**).

## Build & tests

LLVM (example macOS + Homebrew):

```bash
cd duckdb
mkdir -p build && cd build
cmake .. -DDUCKDB_JIT_ENABLED=ON -DLLVM_DIR="$(brew --prefix llvm)/lib/cmake/llvm"
cmake --build . -j4 --target duckdb unittest
```

```bash
./test/unittest "[jit_profiler]"
./test/unittest "[jit_dispatcher]"
./test/unittest "[jit_compiler]"
```

On macOS with LLVM: `DYLD_LIBRARY_PATH="$(brew --prefix llvm)/lib:$DYLD_LIBRARY_PATH"` may be required.

Runtime: **`JITDispatcher::SetEnableJIT(true)`** (and threshold tuning) in your embedding or demo; default is **JIT off**.

## Documentation (`docs/`)

| Document | Contents |
|----------|----------|
| [docs/jit-source-tour.md](docs/jit-source-tour.md) | **Start here** — file map, execution flow, extension notes. |
| [docs/architecture.md](docs/architecture.md) | Diagrams and narrative for the JIT pipeline. |
| [docs/codebase-vs-upstream-duckdb.md](docs/codebase-vs-upstream-duckdb.md) | Diff vs stock DuckDB; integration **status**. |
| [docs/runtime-implementation.md](docs/runtime-implementation.md) | Cache, dispatcher, benchmark API (detail). |
| [docs/part-c-summary.md](docs/part-c-summary.md) | Short Part C / runtime summary. |
| [docs/usage-examples.md](docs/usage-examples.md) | C++ examples. |
| [docs/README.md](docs/README.md) | Index of all docs. |

Upstream DuckDB: **`duckdb/README.md`**.

## Benchmarking results

| Date | Config (JIT on/off, threshold, LLVM version) | Query / workload | Interpreter (ms) | JIT (ms) | Speedup | Notes |
|------|-----------------------------------------------|------------------|------------------|----------|---------|-------|
| | | | | | | |
| | | | | | | |

Export CSV (e.g. **`JITBenchmark::ExportCSV`**) under **`docs/benchmarks/`** and link filenames here.

## Repository layout

```
├── README.md
├── docs/                     # Project documentation (JIT extension)
│   ├── jit-source-tour.md    # Code map + integration points
│   └── benchmarks/           # Optional: drop CSV exports here
├── duckdb/
│   ├── src/execution/jit/
│   └── ...
└── ...
```

---

*Course project (CSCI 543, Spring 2026).*
