# Benchmarking guide

This document describes how to build and run the project’s benchmarks, what each binary measures, and how to interpret the console output (especially `jit_micro_benchmark`).

## What gets compared

All three tools contrast two execution modes on the **same queries and data**:

| Mode | How it is selected |
|------|---------------------|
| **Vectorized baseline** | `JITDispatcher::SetEnableJIT(false)` — DuckDB’s ordinary vectorized interpreter for expressions. |
| **JIT path** | `SetEnableJIT(true)` — hot expressions can compile to native code after crossing a **tuple threshold**, with cache lookup and safe fallback to the interpreter. |

Warm-up and iteration counts differ per tool; the micro-benchmark always does **one** warm-up execution per phase, then averages `repeat` timed runs.

## Build

From the `duckdb` directory:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DDUCKDB_JIT_ENABLED=ON
cmake --build build --target tpch_jit_benchmark tpcds_jit_benchmark jit_micro_benchmark unittest -j4
```

On macOS with Homebrew LLVM you may need:

```bash
export DYLD_LIBRARY_PATH="$(brew --prefix llvm)/lib:$DYLD_LIBRARY_PATH"
```

### Optional: cap worker threads

For reproducible laptop numbers, align with other project docs:

```bash
export DUCKDB_BENCHMARK_THREADS=2
```

The benchmarks read this environment variable and apply `PRAGMA threads=N` on the connection when set.

## Artifact location

Write CSVs under `docs/benchmark/` so results stay versioned and easy to find, for example:

```bash
./build/tools/jit_micro_benchmark 200000 1000 10000 ../docs/benchmark/jit_micro_results.csv
```

(Run these commands from `duckdb/` so `../docs/benchmark/` resolves correctly.)

---

## TPC-H (`tpch_jit_benchmark`)

End-to-end TPC-H at a chosen scale factor, with JIT disabled vs enabled.

```bash
cd duckdb
./build/tools/tpch_jit_benchmark 0.1 3 ../docs/benchmark/tpch_jit_results.csv 1000
```

| Argument | Meaning |
|----------|---------|
| `scale_factor` | TPC-H scale (e.g. `0.1`). |
| `iterations` | Number of timed runs per query after warm-up. |
| `csv_out` | Output CSV path. |
| `jit_tuple_threshold` | Optional; tuples before a compile attempt (default in tool if omitted). |

---

## TPC-DS (`tpcds_jit_benchmark`)

Same pattern for the full TPC-DS template set. You can pass **one or more** thresholds; each value produces a sweep of runs so you can see sensitivity to hotness policy.

```bash
./build/tools/tpcds_jit_benchmark 0.01 3 ../docs/benchmark/tpcds_jit_results.csv 100 1000 10000
```

| Argument | Meaning |
|----------|---------|
| `scale_factor` | TPC-DS scale (e.g. `0.01` for a fast run). |
| `iterations` | Timed runs per query after warm-up. |
| `csv_out` | Output CSV path. |
| `threshold ...` | Optional list of JIT tuple thresholds. If omitted, the tool defaults to three thresholds: `100`, `1000`, `10000`. |

---

## JIT micro-benchmark (`jit_micro_benchmark`)

Source of truth for scenario names and SQL: `duckdb/tools/jit_micro_benchmark.cpp`.

### Command line

```bash
./build/tools/jit_micro_benchmark [rows] [repeat] [threshold] [csv_out]
```

| Argument | Default | Meaning |
|----------|---------|---------|
| `rows` | `1000000` | Rows in table `t`; each scenario scans this many rows (except `tiny_heavy`, which wraps a `LIMIT` subquery). |
| `repeat` | `1` | Timed executions per scenario **per phase** (after one warm-up per phase). Use large values (e.g. `1000`) to average noise; use huge `repeat` with **small** `rows` to stress dispatch and threshold behavior. |
| `threshold` | `1000` | JIT compilation tuple threshold for the run. |
| `csv_out` | `jit_micro_results.csv` | Optional CSV summarizing every scenario. |

The tool bumps the compile cache to **512** entries so `many_tiny` (many distinct expression roots) does not thrash the LRU.

### Scenarios (synthetic + edge cases)

Data: `CREATE TABLE t AS SELECT … FROM range(rows)` with `BIGINT` columns `a`–`e` (deterministic modulo patterns).

| Scenario | Intent |
|----------|--------|
| `projection` | Three arithmetic outputs per row — typical projection-heavy JIT. |
| `filter` | Arithmetic predicate with `AND` — filter-side JIT. |
| `mixed` | Projection expression under an arithmetic filter — combines both paths. |
| `complex` | Single heavy arithmetic expression — often strong JIT speedup when amortized. |
| `many_tiny` | Many cheap arithmetic roots per row — high dispatch / per-root overhead; vectorized engine often wins. |
| `tiny_heavy` | Same kernel as `complex` but over `(SELECT * FROM t LIMIT 8192)` — small chunks: low `jit_pct` until work is hot enough; measures threshold + overhead. |
| `case_expr` | `CASE WHEN` — compares JIT vs simple arithmetic mix. |
| `between_filter` | `BETWEEN` on a column — often shows **no** `JITDispatcher` compile/execute counters because this predicate may not go through the same JIT hooks as the hand-written arithmetic filters; timings can still be similar (baseline vs JIT). |
| `branchy_or` | `OR` + `AND` — more branchy predicate than plain `filter`. |

### Per-scenario console output

For each scenario you will see:

- **Vector avg** / **JIT avg** — mean wall time in ms for the full `conn.Query(sql)` (including planning where applicable), after warm-up, averaged over `repeat`.
- **Speedup** — `vector_avg / jit_avg` (values below `1.0` mean JIT path was slower for that scenario on your machine).
- **JITDispatcher stats** — live counters: JIT vs interpreter executions, compilation attempts/successes/failures, threshold.

### Summary table (end of run)

The closing block defines the columns explicitly:

- **`jit_pct`** — `jit_executions / (jit_executions + interpreter_executions)` from the dispatcher after the **JIT-enabled** phase only (0% when no dispatcher-driven JIT ran, e.g. some `between_filter` runs).
- **`cmp_att` / `cmp_ok` / `cmp_fail`** — compile attempts, LLVM successes, and failures (`CanCompile` / null paths).
- **`vec_ms` / `jit_ms` / `speedup`** — same averages as the per-scenario lines; handy for copying into reports.

#### Example full summary (one machine)

The table below is a **real end-of-run summary** from a single invocation. It is **not** a specification: absolute milliseconds and speedups change with CPU, thermal state, `DUCKDB_BENCHMARK_THREADS`, and OS load. Use it as a reference for **column layout** and **qualitative patterns** (for example high `jit_pct` with slowdown on `many_tiny`, or `between_filter` with `jit_pct` 0 and no compile events).

Run from `duckdb/`:

```bash
./build/tools/jit_micro_benchmark 200000 1000 10000 ../docs/benchmark/jit_micro_results.csv
```

That means **200000** rows in `t`, **1000** timed executions per scenario per phase (after warm-up), and a JIT tuple threshold of **10000**.

| scenario | jit_pct | cmp_att | cmp_ok | cmp_fail | vec_ms | jit_ms | speedup |
|----------|--------:|--------:|-------:|---------:|-------:|-------:|--------:|
| projection | 99.99 | 3 | 3 | 0 | 4.688 | 2.673 | 1.75 |
| filter | 99.99 | 1 | 1 | 0 | 1.910 | 1.170 | 1.63 |
| mixed | 99.99 | 1 | 1 | 0 | 1.717 | 2.359 | 0.73 |
| complex | 99.96 | 1 | 1 | 0 | 5.619 | 2.048 | 2.74 |
| many_tiny | 100.00 | 12 | 12 | 0 | 1.610 | 7.278 | 0.22 |
| tiny_heavy | 7.68 | 1 | 1 | 0 | 1.312 | 1.090 | 1.20 |
| case_expr | 100.00 | 1 | 1 | 0 | 0.601 | 1.076 | 0.56 |
| between_filter | 0.00 | 0 | 0 | 0 | 0.266 | 0.263 | 1.01 |
| branchy_or | 99.99 | 3 | 3 | 0 | 1.113 | 1.825 | 0.61 |

### Micro-benchmark CSV schema

The written CSV columns are:

`scenario`, `rows`, `repeat`, `threshold`, `jit_usage_pct`, `compile_attempts`, `compile_successes`, `compile_failures`, `vectorized_avg_ms`, `jit_avg_ms`, `speedup`

### Stress: many queries, few rows

To hammer threshold and dispatch without scanning huge tables:

```bash
./build/tools/jit_micro_benchmark 1000 1000000 10000 ../docs/benchmark/jit_micro_million_repeat_th10000.csv
```

### Compile logging

Print per-expression compile / reject lines from the dispatcher:

```bash
JIT_LOG_COMPILE=1 ./build/tools/jit_micro_benchmark 50000 1 1000
```

---

## Stored benchmark artifacts

Keep generated CSVs and logs under `docs/benchmark/` (TPC-H, TPC-DS, micro sweeps, stress logs). The repository may already contain sample outputs for comparison across commits.
