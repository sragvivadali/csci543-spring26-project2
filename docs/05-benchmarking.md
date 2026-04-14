# Benchmark Guide

This project includes three benchmark entry points:

- `tpch_jit_benchmark`
- `tpcds_jit_benchmark`
- `jit_micro_benchmark`

## Build

```bash
cd duckdb
cmake -B build -DCMAKE_BUILD_TYPE=Release -DDUCKDB_JIT_ENABLED=ON
cmake --build build --target tpch_jit_benchmark tpcds_jit_benchmark jit_micro_benchmark -j4
```

Optional runtime thread cap:

```bash
export DUCKDB_BENCHMARK_THREADS=2
```

## TPC-H

```bash
./build/tools/tpch_jit_benchmark 0.1 3 ../docs/benchmark/tpch_jit_results.csv 1000
```

Args: `sf iterations csv_out threshold`

## TPC-DS

```bash
./build/tools/tpcds_jit_benchmark 0.01 3 ../docs/benchmark/tpcds_jit_results.csv 100 1000 10000
```

Args: `sf iterations csv_out thresholds...`

## Microbenchmark (Projection / Filter / Mixed)

```bash
./build/tools/jit_micro_benchmark 1000000 5 1000 ../docs/benchmark/jit_micro_results.csv
```

Args: `rows repeat threshold csv_out`

### Million-query stress pattern

Use small rows + high repeat to stress threshold behavior:

```bash
./build/tools/jit_micro_benchmark 1000 1000000 10000 ../docs/benchmark/jit_micro_million_repeat_th10000.csv
```

### Compile event logging

```bash
JIT_LOG_COMPILE=1 ./build/tools/jit_micro_benchmark 50000 1 1000
```

## Stored Benchmark Artifacts

All generated benchmark CSV/log files should be kept under `docs/benchmark/`.
