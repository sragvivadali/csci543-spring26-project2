# Overview

This project extends a vendored DuckDB tree with a runtime expression JIT pipeline.

## Goals

- Identify hot expressions at runtime.
- Compile supported expression trees with LLVM.
- Reuse compiled code via a cache.
- Dispatch between JIT and interpreter with measurable statistics.

## Core Components

- `JITProfiler`: fingerprints expressions and tracks hotness (`Record`, `GetCount`).
- `JITCompiler`: validates and compiles supported expression trees (`CanCompile`, `Compile`).
- `JITCache`: thread-safe fingerprint-keyed LRU cache for compiled wrappers.
- `JITDispatcher`: runtime decision point (`TryExecuteJIT`) and compile triggering.
- `ExpressionExecutor` hooks: profiling and JIT dispatch entry points for execute/select paths.

## Current Capability Snapshot

- JIT supports arithmetic/comparison/conjunction/cast trees over supported physical types.
- Additional support was added for wider integer flows (`INT128` / `UINT128`), unary `+/-`, `//`, and depth-0 `BOUND_COLUMN_REF`.
- JIT is runtime-configurable (`SetEnableJIT`, `SetCompilationThreshold`).

## Build Prerequisite

For real JIT compilation, build DuckDB with LLVM enabled:

```bash
cd duckdb
cmake -B build -DCMAKE_BUILD_TYPE=Release -DDUCKDB_JIT_ENABLED=ON
cmake --build build --target duckdb unittest -j4
```

Without LLVM/JIT build flags, compiler calls are stubs and execution remains interpreter-only.
