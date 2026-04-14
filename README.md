# JIT Compilation for Arithmetic Expressions in DuckDB

![Build Status](https://img.shields.io/badge/build-manual%20build%20passing-brightgreen)
![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![LLVM](https://img.shields.io/badge/LLVM-22.1.1-purple)
![DuckDB](https://img.shields.io/badge/DuckDB-vendored%20fork-6c5ce7)
![License](https://img.shields.io/badge/license-MIT-green)

This project extends DuckDB with a lightweight runtime JIT compilation pipeline for arithmetic expression evaluation. The motivation is simple: DuckDB's vectorized execution engine is fast, but repeatedly interpreting the same arithmetic-heavy expressions in filters and projections still incurs avoidable overhead. Our system profiles expression hotness, selectively compiles supported trees with LLVM, caches the compiled code, and safely falls back to DuckDB's interpreter when compilation is unsupported or not yet worthwhile.

**Team:** Sragvi Vadali, Joonho Son, Michael Sipes

## Table of Contents

- [Overview](#overview)
- [System Architecture](#system-architecture)
- [Implementation Status](#implementation-status)
- [Build and Run](#build-and-run)
- [Benchmarks](#benchmarks)
- [Documentation](#documentation)
- [Repository Layout](#repository-layout)

## Overview

### Problem Statement

DuckDB evaluates expressions through a high-quality vectorized interpreter, but arithmetic-heavy workloads can still pay repeated interpreter overhead, especially in hot projection and filter paths. That overhead compounds when the same expression tree is executed across many chunks or benchmark iterations.

### Proposed Solution

We implemented a lightweight selective JIT pipeline using LLVM:

- profile expression hotness at runtime
- compile only supported expression trees after they cross a hotness threshold
- cache compiled code by structural fingerprint
- preserve correctness with interpreter fallback for unsupported states

### Architecture at a Glance

The system is organized around three core research components:

1. **Execution Profiler** — tracks how often an expression runs and identifies hot candidates.
2. **JIT Compiler** — lowers supported bound expressions to LLVM IR and native code.
3. **Compiled Code Cache** — stores reusable compiled wrappers keyed by expression fingerprint.

Conceptually, the architecture is:

```text
Expression batch
    ↓
Profiler (hotness / fingerprint)
    ↓
JIT compiler (on hot miss)
    ↓
Compiled code cache
    ↓
Execute compiled function or fall back to interpreter
```

### Key Design Decisions

- **Hot threshold:** compilation is triggered only after an expression has accumulated enough tuples to justify the overhead.
- **Expression fingerprinting:** structurally equivalent bound expressions map to the same cache key.
- **Safe fallback:** unsupported expression types, unsupported execution states, and compilation failures always return to DuckDB's interpreter path.

## System Architecture

### Execution Flow

The runtime flow is:

1. Expression is evaluated in DuckDB's execution engine.
2. The system checks the compiled code cache for a matching fingerprint.
3. If there is a cache hit, it executes the compiled function.
4. If there is a cache miss, the profiler increments the execution count for that expression.
5. If the expression is still cold, DuckDB uses the normal interpreter.
6. If the expression is hot, the system attempts LLVM compilation.
7. On success, the compiled function is inserted into the cache and executed.
8. On failure, execution safely falls back to the interpreter.

### Detailed Flow Diagram

```text
┌─────────────────────────────────────────────────────────────┐
│                 Expression evaluation in DuckDB            │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
                   ┌───────────────────────────┐
                   │       JITProfiler         │
                   │ Record(expr, count)       │
                   └──────────────┬────────────┘
                                  │
                                  ▼
                  ┌─────────────────────────────────┐
                  │    JITDispatcher::TryExecuteJIT │
                  └──────────────┬──────────────────┘
                                 │
                     ┌───────────┴───────────┐
                     │                       │
                     ▼                       ▼
          ┌────────────────────┐   ┌──────────────────────┐
          │ Cache hit          │   │ Cache miss           │
          │ JITCache::Lookup   │   │ Check hot threshold  │
          └─────────┬──────────┘   └──────────┬───────────┘
                    │                         │
                    ▼                         ▼
         ┌──────────────────┐        ┌───────────────────┐
         │ Execute compiled │        │ Cold → interpreter│
         │ function         │        └───────────────────┘
         └──────────────────┘                  ▲
                                               │
                                      Hot      │
                                       ▼       │
                              ┌────────────────────────┐
                              │ LLVM JIT compilation   │
                              │ + insert into cache    │
                              └──────────┬─────────────┘
                                         │
                                         ▼
                                  ┌──────────────┐
                                  │ Execute JIT  │
                                  └──────────────┘
```

### Core Components

#### 1. Profiler

The profiler fingerprints bound expressions and tracks hotness counts. It is responsible for deciding when a tree becomes hot enough to justify a compile attempt.

Key ideas:

- structural hashing of bound expression trees
- tuple-count accumulation instead of naive call counting
- threshold synchronization with dispatcher policy

#### 2. JIT Compiler

The compiler uses LLVM to generate IR and native code for supported expression trees.

Main responsibilities:

- validate compilable expression structure
- lower arithmetic/comparison trees to LLVM IR
- preserve null propagation behavior
- return native functions that operate on DuckDB vector data

#### 3. Code Cache

The code cache stores compiled functions by expression fingerprint and reuses them across future executions.

Key properties:

- thread-safe lookup and insertion
- LRU-style cache management
- reuse of compiled wrappers after the first successful compile

### Supported Types

The original proposal focused on arithmetic expressions over:

- `INT32`
- `INT64`
- `FLOAT`

The current implementation supports a broader numeric set in practice. Based on the current `JITCompiler` support table, supported physical types include:

- `BOOL`
- `UINT8`, `INT8`
- `UINT16`, `INT16`
- `UINT32`, `INT32`
- `UINT64`, `INT64`
- `UINT128`, `INT128`
- `FLOAT`
- `DOUBLE`

This broader support was added to match real DuckDB expression widening behavior. The research focus, however, remains arithmetic-heavy numeric expressions of the kind described in the original proposal.

## Implementation Status

The current codebase includes all major research pieces:

- **Profiler**: active and wired into execution paths
- **JIT compiler**: implemented with LLVM-backed compilation when `DUCKDB_JIT_ENABLED=ON`
- **Code cache**: implemented and used by the dispatcher
- **Dispatcher**: performs lookup, compile-on-hot-miss, execution accounting, and fallback
- **Execution hooks**: active in dense function execution and dense comparison-filter execution

Recent implementation work also addressed several practical DuckDB edge cases:

- filter predicate JIT integration
- sparse input-column materialization
- `INT128` / `UINT128` widening support
- unary `+` / `-`
- integer `//`
- depth-0 `BOUND_COLUMN_REF`

## Build and Run

### Build

From the `duckdb` directory:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DDUCKDB_JIT_ENABLED=ON
cmake --build build --target duckdb unittest tpch_jit_benchmark tpcds_jit_benchmark jit_micro_benchmark -j4
```

On macOS with Homebrew LLVM, you may need:

```bash
export DYLD_LIBRARY_PATH="$(brew --prefix llvm)/lib:$DYLD_LIBRARY_PATH"
```

### Tests

```bash
cd duckdb/build
./test/unittest "[jit_profiler]"
./test/unittest "[jit_dispatcher]"
./test/unittest "[jit_compiler]"
```

### Runtime Control

JIT is off by default and can be enabled through:

- `JITDispatcher::SetEnableJIT(true)`
- `JITDispatcher::SetCompilationThreshold(...)`

For benchmark runs, it is often useful to cap DuckDB worker threads:

```bash
export DUCKDB_BENCHMARK_THREADS=2
```

## Benchmarks

We evaluate the system with three workload families:

- **TPC-H**
- **TPC-DS**
- **Custom microbenchmarks**

### TPC-H

```bash
cd duckdb
./build/tools/tpch_jit_benchmark 0.1 3 ../docs/benchmark/tpch_jit_results.csv 1000
```

### TPC-DS

```bash
cd duckdb
./build/tools/tpcds_jit_benchmark 0.01 3 ../docs/benchmark/tpcds_jit_results.csv 100 1000 10000
```

### Microbenchmarks

```bash
cd duckdb
./build/tools/jit_micro_benchmark 100000 5 1000 ../docs/benchmark/jit_micro_results.csv
```

The microbenchmark suite includes:

- pure arithmetic projection
- arithmetic filter
- mixed filter + projection

### Results Summary

| Scenario | Vectorized | JIT | Speedup |
|---|---:|---:|---:|
| Projection | 0.184ms | 0.168ms | 1.09x |
| Filter | 0.121ms | 0.114ms | 1.05x |
| Mixed | 0.116ms | 0.123ms | 0.94x |

Raw benchmark outputs are stored in `docs/benchmark/`.

## Documentation

Project documentation is organized in `docs/`:

- [`docs/README.md`](docs/README.md) - reading order and documentation home
- [`docs/01-overview.md`](docs/01-overview.md) - project-level overview
- [`docs/02-architecture.md`](docs/02-architecture.md) - architecture and diagrams
- [`docs/03-source-tour.md`](docs/03-source-tour.md) - source file guide
- [`docs/04-runtime-details.md`](docs/04-runtime-details.md) - runtime semantics, challenges, and fixes
- [`docs/05-benchmarking.md`](docs/05-benchmarking.md) - benchmark commands
- [`docs/06-challenges-and-open-issues.md`](docs/06-challenges-and-open-issues.md) - unresolved issues and future work

## Repository Layout

```text
├── README.md
├── docs/
│   ├── README.md
│   ├── 01-overview.md
│   ├── 02-architecture.md
│   ├── 03-source-tour.md
│   ├── 04-runtime-details.md
│   ├── 05-benchmarking.md
│   ├── 06-challenges-and-open-issues.md
│   └── benchmark/
├── duckdb/
│   ├── src/execution/jit/
│   ├── src/execution/expression_executor/
│   ├── tools/
│   └── test/api/
└── ...
```

---

Course project for CSCI 543 (Spring 2026).
