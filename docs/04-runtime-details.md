# Runtime Details

## Dispatcher Semantics

`JITDispatcher::TryExecuteJIT` follows this order:

1. Check JIT enabled flag.
2. Check cache for compiled function.
3. If cache miss and expression is hot enough, attempt compile.
4. Execute compiled wrapper on success.
5. Fall back to interpreter otherwise.

## Profiling Semantics

- `Record(expr, count)` is called before dispatch in execution paths.
- Profiling increments are not limited to cache misses.
- Threshold synchronization uses `SetCompilationThreshold` on dispatcher and profiler hotness policy.

## Compilation Counters

Dispatcher reports:

- `compilation_attempts`
- `compilation_successes`
- `compilation_failures`

A single fingerprint can compile multiple times across queries when benchmark code clears cache between runs.

## Notes on Supported Expressions

Current compiler implementation supports core arithmetic/comparison/conjunction/cast paths over supported physical types, including recent additions for:

- integer widening paths (`INT128` / `UINT128`)
- unary arithmetic function forms (`+`, `-`)
- integer floor division (`//`)
- depth-0 `BOUND_COLUMN_REF`

Correlated planner refs (`BOUND_COLUMN_REF` with `depth > 0`) still intentionally fall back.

## Challenges and Edge Cases

DuckDB's execution engine is highly vectorized and has multiple expression entry points, so several practical pitfalls showed up during integration.

### 1. JIT hooks were not all in one place

**Challenge:** projection-style expression evaluation and filter `Select(...)` paths do not all flow through one identical code path. It is easy to profile or JIT only `Execute(...)` and miss predicate filtering entirely.

**What we changed:**

- kept profiling at the executor entry point
- added JIT-aware comparison filter handling in `execute_comparison.cpp`
- preserved the existing vectorized fallback path unchanged when JIT does not apply

**Impact:** filter predicates now participate in the JIT pipeline instead of being silently interpreter-only.

### 2. JIT kernels assume dense row iteration

**Challenge:** the generated LLVM loop iterates `0..count-1` and does not understand an arbitrary DuckDB `SelectionVector`. That means JIT cannot safely consume every executor state the interpreter can.

**What we changed:**

- only attempt JIT on dense scans (`sel == nullptr`)
- keep the interpreter for paths with active selection vectors
- for comparison `Select(...)`, execute JIT into a temporary boolean vector and then reconstruct `true_sel` / `false_sel`

**Impact:** JIT remains correct without needing a second code generator for sparse row iteration.

### 3. Flattening every input column was unsafe

**Challenge:** the early wrapper materialized every column in the chunk before calling native code. In DuckDB that can touch vectors unrelated to the expression, including ones whose physical representation is not safe for this JIT path. This surfaced as failures like `Invalid PhysicalType for GetTypeIdSize`.

**What we changed:**

- switched from dense input arrays to **sparse column pointers**
- collect only referenced columns from the expression tree
- flatten only those referenced columns in the JIT wrapper
- keep a defensive check for invalid physical types on the columns that are actually used

**Impact:** unrelated chunk columns no longer cause JIT failures for otherwise compilable expressions.

### 4. Planner refs and executor refs are different expression classes

**Challenge:** DuckDB may carry `BOUND_COLUMN_REF` before later rewrites turn it into `BOUND_REF`. Supporting only one of them reduces practical JIT coverage.

**What we changed:**

- added `BOUND_COLUMN_REF` to the compilable set
- mapped depth-0 planner refs through `binding.column_index` to the same sparse input array used by `BOUND_REF`

**Remaining caveat:** correlated refs (`depth > 0`) still fall back because they do not map to the current chunk in the same simple way.

### 5. Null semantics had to match DuckDB exactly

**Challenge:** DuckDB predicates do not treat `NULL` as `true`, and comparison `Select(...)` expects correct validity behavior plus correct true/false selection reconstruction.

**What we changed:**

- native JIT functions emit both value bytes and validity bytes
- wrapper propagates validity back into DuckDB vectors
- comparison-select JIT path treats invalid rows as not selected when building `true_sel` / `false_sel`

**Impact:** JIT preserves the same null semantics as the interpreter for the supported paths.

### 6. Type widening in DuckDB is broader than a simple `int64/double` model

**Challenge:** arithmetic in DuckDB can widen into `INT128` / `UINT128`, or mix signed and unsigned integer flows in ways that a minimal compiler rejects.

**What we changed:**

- extended `CanCompile` and LLVM type lowering to wider integer types
- added signedness-aware promotion logic
- used signed vs unsigned division/comparison operations where appropriate

**Impact:** previously rejected arithmetic-heavy filters and projections now compile successfully.

### 7. Unary operators and floor division were easy to miss

**Challenge:** an initial implementation that assumes all function expressions are binary misses valid DuckDB function forms such as unary `-`, and arithmetic profiling included `//` even though codegen did not.

**What we changed:**

- added single-child unary `+` / `-` handling
- added integer `//` codegen with signed floor semantics

**Impact:** compiler behavior now better matches the expression shapes seen in real plans and benchmarks.

### 8. Benchmark behavior can be misleading without context

**Challenge:** benchmark tools often clear cache between queries, which means the same fingerprint may compile more than once. Also, per-attempt compile logging can overwhelm terminal output on large scans.

**What we changed:**

- documented that repeated compile events across queries are expected when cache is reset
- gated detailed compile/reject logging behind `JIT_LOG_COMPILE=1`
- added per-scenario dispatcher stats in the microbenchmark

**Impact:** logs and benchmark numbers are easier to interpret correctly.
