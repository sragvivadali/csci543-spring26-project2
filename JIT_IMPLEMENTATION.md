# JIT profiler implementation overview (CSCI 543 project)

This document now reflects the **profiler-only** setup in this repository: expression fingerprinting + count cache.

---

## Architecture (current state)

1. **`JITProfiler::Fingerprint(expr)`** computes a 64-bit structural hash of a bound expression tree.
2. **`JITProfiler::Record(expr, tuple_count)`** increments `counts[fingerprint] += tuple_count`.
3. **`ExpressionExecutor::Execute(...)`** calls `Record` for non-empty batches so execution volume is accumulated during normal query execution.
4. **`JITProfiler::PrintStats()`** prints the internal hash table (`fingerprint -> total evaluated tuples`).

No LLVM compiler, dispatcher, or native function cache is active in the current codebase.

---

## Files and where logic lives

### Public headers (`src/include/duckdb/execution/jit/`)

| File | Role |
|------|------|
| `duckdb/src/include/duckdb/execution/jit/jit_profiler.hpp` | API + behavior notes (fingerprint semantics, thread safety, instrumentation points). |

### Thin wrappers beside sources (`src/execution/jit/*.hpp`)

| File | Role |
|------|------|
| `duckdb/src/execution/jit/jit_profiler.hpp` | Thin wrapper include for source-side path compatibility. |

### Implementations (`src/execution/jit/`)

| File | Role |
|------|------|
| `duckdb/src/execution/jit/jit_profiler.cpp` | FNV-1a tree walk + thread-safe `unordered_map<uint64_t, uint64_t>` counter updates/reads/printing. |

### Executor integration

| File | Change |
|------|--------|
| `duckdb/src/execution/expression_executor.cpp` | After `count == 0` guard, it directly calls `JITProfiler::GetInstance().Record(expr, count)` before normal expression dispatch. |

### Build system

No LLVM linkage is required for the profiler-only path.

### Tests (`test/api/`)

| File | Role |
|------|------|
| `duckdb/test/api/test_jit_profiler.cpp` | Fingerprint and `Record` behavior tests. |
| `duckdb/test/api/CMakeLists.txt` | Includes `test_jit_profiler.cpp` in `test_api` object list. |

### Tools (optional)

| File | Role |
|------|------|
| `duckdb/tools/jit_profiler_demo.cpp` | Standalone executable that runs SQL and prints the profiler hash table. |

---

## Interpreting output

`PrintStats()` outputs lines of this form:

- `Expression fingerprint <u64> evaluated <count> times`

Meaning:

- `fingerprint`: structural expression identity (hash), not SQL text.
- `count`: total tuples processed through that expression across all recorded batches.
- Large counts (e.g. 1,000,000+) usually correspond to row-wise hot operators.
- Small counts (e.g. 2/3/6) are often setup/scalar-only expression evaluations.

---

## Running profiler tests

From the CMake build directory:

```bash
cmake --build . --target unittest -j 8
./test/unittest "[jit_profiler]"
```

---

## Summary

Current implementation is a clean **hash-based expression profiler**:

- fingerprints bound expression trees,
- stores counts in a thread-safe hash table,
- records from the normal executor path,
- and exposes counts via `GetCount`/`PrintStats` for analysis.
