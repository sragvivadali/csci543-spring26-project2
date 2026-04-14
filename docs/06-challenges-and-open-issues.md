# Challenges and Open Issues

This page documents the practical challenges we encountered while integrating JIT into DuckDB, what we changed to address them, and what remains intentionally out of scope or incomplete.

## Challenges We Addressed

### 1. Filter predicates were not actually attempting JIT

**Challenge:** early JIT integration focused on expression execution paths, while filter `Select(...)` paths still only recorded profiler data and fell back to vectorized execution.

**What we did:**

- added JIT-aware handling in comparison filter execution
- preserved the existing interpreter/vectorized path as fallback
- reconstructed `true_sel` / `false_sel` from JIT-produced boolean results

**Result:** arithmetic-heavy predicate filters can now participate in the JIT pipeline.

### 2. JIT wrappers were touching unrelated columns

**Challenge:** materializing every column in a chunk caused failures even when the expression only referenced a few of them. In practice, DuckDB chunks can contain columns whose physical shape is not safe for this JIT path.

**What we did:**

- switched to sparse column pointer population
- flattened only the columns actually referenced by the expression tree
- added defensive checks on referenced inputs rather than the whole chunk

**Result:** JIT execution became much more robust on realistic queries and no longer failed just because an unrelated column had an awkward representation.

### 3. DuckDB type widening exceeded a small arithmetic subset

**Challenge:** apparently simple arithmetic could widen into `INT128` / `UINT128`, and those trees were being rejected by `CanCompile`.

**What we did:**

- added support for wider integer physical types
- implemented signedness-aware promotion
- added signed/unsigned arithmetic and comparison lowering

**Result:** previously rejected arithmetic-heavy filters and projections now compile successfully.

### 4. Some common expression forms were missing

**Challenge:** the compiler initially assumed only binary arithmetic function forms, which excluded unary `+/-` and `//` despite those appearing in real DuckDB expression trees.

**What we did:**

- added unary `+` / unary `-` support
- added integer `//` support with signed floor-division semantics

**Result:** expression coverage is closer to what the DuckDB executor actually sees in practice.

### 5. Planner refs and executor refs are not the same node type

**Challenge:** DuckDB uses both `BOUND_REF` and `BOUND_COLUMN_REF` in different stages of planning/execution, so supporting only one reduced JIT coverage.

**What we did:**

- accepted `BOUND_COLUMN_REF` in `CanCompile`
- mapped depth-0 planner refs through `binding.column_index`

**Result:** more planner-originated expression trees can compile without requiring earlier rewrites.

### 6. Benchmark output could be misleading without context

**Challenge:** repeated cache clears can make the same fingerprint compile multiple times, and verbose rejection logging can overwhelm long-running benchmarks.

**What we did:**

- documented repeated compile attempts across benchmark runs
- added optional compile logging via `JIT_LOG_COMPILE=1`
- added per-scenario dispatcher stats in the microbenchmark

**Result:** benchmark output is easier to interpret and debug.

## Open Issues and Things We Could Not Fully Achieve

### 1. Correlated `BOUND_COLUMN_REF` is still not compiled

- depth-0 planner refs are supported
- correlated refs (`depth > 0`) still fall back because they do not map cleanly to the current chunk

### 2. Floating-point `//` semantics are still a caveat

- integer floor division is implemented correctly
- exact floating floor-division semantics are not fully modeled in the current lowering

### 3. Dispatcher statistics are not fully thread-safe

- counters such as `jit_executions` and `interpreter_executions` are simple shared counters
- under heavy parallel query execution they can show race noise

### 4. Benchmark automation is still manual

- there is no built-in script that runs a full scale-factor matrix in one command
- benchmark artifacts are produced correctly, but orchestration is still manual

### 5. Compile-time cost is not surfaced strongly enough

- cache insertions do not yet produce a polished benchmark-facing compile-time report
- this makes it harder to separate execution speedup from one-time compilation overhead

### 6. JIT still intentionally falls back in several executor states

- dense scans are the primary supported case
- active selection vectors and other complex execution states still rely on the interpreter path for correctness and simplicity

## Summary

The main engineering work succeeded in making the JIT path more realistic for DuckDB workloads: predicate filters now attempt JIT, wider integer arithmetic compiles, and several executor/planner mismatches were resolved. The remaining open issues are mostly around edge semantics, correlated references, threading polish, and benchmark/reporting completeness rather than core end-to-end viability.
