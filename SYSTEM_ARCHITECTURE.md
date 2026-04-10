# JIT System Architecture

## Overall System Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         Query Execution                          │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│                    ExpressionExecutor                            │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Execute(Expression &expr, DataChunk &input, Vector &out) │  │
│  └───────────────────────┬───────────────────────────────────┘  │
└──────────────────────────┼──────────────────────────────────────┘
                           │
                           ▼
        ┌──────────────────────────────────────┐
        │      JITProfiler::Record()           │
        │  (Track execution count - Part A)    │
        └──────────────────────────────────────┘
                           │
                           ▼
        ┌──────────────────────────────────────┐
        │   JITDispatcher::TryExecuteJIT()     │
        │    (Decide JIT vs Interpreter)       │
        │           (Part C)                   │
        └──────────────┬───────────────────────┘
                       │
         ┌─────────────┴─────────────┐
         │                           │
         ▼                           ▼
    ┌────────┐                 ┌──────────┐
    │ JIT    │                 │ Check    │
    │Enabled?│                 │ Cache    │
    └───┬────┘                 └────┬─────┘
        │ No                        │
        │                           ▼
        │              ┌─────────────────────────┐
        │              │  JITCache::Lookup()     │
        │              │      (Part C)           │
        │              └──────────┬──────────────┘
        │                         │
        │              ┌──────────┴──────────┐
        │              │                     │
        │              ▼                     ▼
        │         ┌─────────┐          ┌─────────┐
        │         │  Found  │          │Not Found│
        │         └────┬────┘          └────┬────┘
        │              │                    │
        │              ▼                    ▼
        │      ┌───────────────┐    ┌──────────────┐
        │      │ Execute JIT   │    │ Check if Hot │
        │      │   Function    │    │  (Profiler)  │
        │      └───────────────┘    └──────┬───────┘
        │                                   │
        │                        ┌──────────┴──────────┐
        │                        │                     │
        │                        ▼                     ▼
        │                   ┌─────────┐          ┌─────────┐
        │                   │   Hot?  │          │Not Hot  │
        │                   └────┬────┘          └────┬────┘
        │                        │                    │
        │                        ▼                    │
        │              ┌──────────────────┐           │
        │              │  TryCompile()    │           │
        │              │   (Part B)       │           │
        │              └────────┬─────────┘           │
        │                       │                     │
        │              ┌────────┴────────┐            │
        │              │                 │            │
        │              ▼                 ▼            │
        │         ┌─────────┐      ┌─────────┐       │
        │         │ Success │      │ Failure │       │
        │         └────┬────┘      └────┬────┘       │
        │              │                │            │
        │              ▼                │            │
        │      ┌───────────────┐        │            │
        │      │ Insert Cache  │        │            │
        │      │ Execute JIT   │        │            │
        │      └───────────────┘        │            │
        │                               │            │
        └───────────────────────────────┴────────────┘
                                        │
                                        ▼
                            ┌───────────────────────┐
                            │ Fall back to          │
                            │ Interpreter           │
                            └───────────────────────┘
```

---

## Component Interaction Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                          Part A: Profiler                        │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  JITProfiler                                              │  │
│  │  • Fingerprint(expr) → uint64_t                          │  │
│  │  • Record(expr, count)                                    │  │
│  │  • GetCount(expr) → uint64_t                             │  │
│  │  • PrintStats()                                           │  │
│  └───────────────────────────────────────────────────────────┘  │
└──────────────────────────────┬──────────────────────────────────┘
                               │ Uses fingerprint
                               │ Provides execution counts
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                          Part C: Runtime                         │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  JITCache                                                 │  │
│  │  • Lookup(expr) → JITCompiledFunction                    │  │
│  │  • Insert(expr, func, compile_time)                      │  │
│  │  • LRU eviction                                           │  │
│  │  • Statistics tracking                                    │  │
│  └───────────────────────────────────────────────────────────┘  │
│                               ▲                                  │
│                               │ Stores/retrieves                 │
│                               │ compiled functions               │
│  ┌───────────────────────────┴───────────────────────────────┐  │
│  │  JITDispatcher                                            │  │
│  │  • TryExecuteJIT(expr, ...) → bool                       │  │
│  │  • SetEnableJIT(bool)                                     │  │
│  │  • SetCompilationThreshold(count)                         │  │
│  │  • TryCompile(expr) → bool [STUB for Part B]            │  │
│  └───────────────────────────┬───────────────────────────────┘  │
│                               │                                  │
│  ┌───────────────────────────┴───────────────────────────────┐  │
│  │  JITBenchmark                                             │  │
│  │  • RunQuery(query, iterations) → BenchmarkResult         │  │
│  │  • RunBenchmarkSuite(queries)                             │  │
│  │  • PrintReport()                                          │  │
│  │  • ExportCSV(filename)                                    │  │
│  └───────────────────────────────────────────────────────────┘  │
└──────────────────────────────┬──────────────────────────────────┘
                               │ Calls TryCompile()
                               │ Inserts into cache
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Part B: LLVM Compiler                     │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  LLVMCodeGenerator (Person B implements this)            │  │
│  │  • GenerateIR(expr) → llvm::Module*                      │  │
│  │  • CompileToNative(module) → JITCompiledFunction         │  │
│  │  • Arithmetic expressions (add, sub, mul, div)           │  │
│  │  • Comparison expressions (>, <, =, !=, >=, <=)          │  │
│  │  • Boolean expressions (AND, OR, NOT)                    │  │
│  │  • Type promotion and null handling                       │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Data Flow Diagram

```
Query: "SELECT a + b FROM table WHERE a > 100"
│
├─ Parse & Bind
│   └─ Creates BoundExpressions
│
├─ Physical Plan
│   └─ ExpressionExecutor::Execute()
│
├─ For each expression:
│   │
│   ├─ JITProfiler::Record(expr, tuple_count)
│   │   └─ counts[Fingerprint(expr)] += tuple_count
│   │
│   ├─ JITDispatcher::TryExecuteJIT(expr, ...)
│   │   │
│   │   ├─ Is JIT enabled? → No: return false (use interpreter)
│   │   │                  → Yes: continue
│   │   │
│   │   ├─ JITCache::Lookup(expr)
│   │   │   └─ hash = Fingerprint(expr)
│   │   │   └─ return cache[hash].function (or nullptr)
│   │   │
│   │   ├─ Found in cache?
│   │   │   └─ Yes: Execute compiled function, return true
│   │   │   └─ No: continue
│   │   │
│   │   ├─ Check if hot:
│   │   │   └─ count = JITProfiler::GetCount(expr)
│   │   │   └─ if (count >= threshold) → TryCompile(expr)
│   │   │
│   │   └─ TryCompile(expr)  [Person B implements]
│   │       │
│   │       ├─ Generate LLVM IR
│   │       ├─ Compile to native code
│   │       ├─ JITCache::Insert(expr, func, compile_time)
│   │       └─ return success
│   │
│   └─ If TryExecuteJIT returned false:
│       └─ Use interpreter (existing code)
│
└─ Return results
```

---

## Cache Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                          JITCache                                │
│                                                                  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │  LRU List (most recent at front)                          │ │
│  │  ┌──────┐    ┌──────┐    ┌──────┐    ┌──────┐           │ │
│  │  │ FP 1 │◄──►│ FP 2 │◄──►│ FP 3 │◄──►│ FP 4 │           │ │
│  │  └──────┘    └──────┘    └──────┘    └──────┘           │ │
│  │     ▲                                        ▲            │ │
│  │     │ Most Recent                   Least Recent          │ │
│  │     │                                (evict from here)    │ │
│  └─────┼────────────────────────────────────────┼────────────┘ │
│        │                                        │              │
│  ┌─────┴────────────────────────────────────────┴────────────┐ │
│  │  Hash Map: fingerprint → (entry, list_iterator)          │ │
│  │  ┌──────────────────────────────────────────────────────┐│ │
│  │  │ FP 1 → (JITCacheEntry, iterator to "FP 1" in list)  ││ │
│  │  │ FP 2 → (JITCacheEntry, iterator to "FP 2" in list)  ││ │
│  │  │ FP 3 → (JITCacheEntry, iterator to "FP 3" in list)  ││ │
│  │  │ FP 4 → (JITCacheEntry, iterator to "FP 4" in list)  ││ │
│  │  └──────────────────────────────────────────────────────┘│ │
│  └───────────────────────────────────────────────────────────┘ │
│                                                                  │
│  JITCacheEntry:                                                 │
│  • JITCompiledFunction function                                 │
│  • uint64_t fingerprint                                         │
│  • idx_t hit_count                                              │
│  • idx_t compile_time_us                                        │
│                                                                  │
│  Operations:                                                    │
│  • Lookup(expr): O(1) hash lookup                              │
│  • Insert(expr, func): O(1) insert + evict if full            │
│  • Touch(fp): O(1) move to front of LRU list                  │
│  • EvictLRU(): O(1) remove from back of list                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Benchmark Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                      JITBenchmark::RunQuery()                    │
└────────────────────────────┬────────────────────────────────────┘
                             │
         ┌───────────────────┴───────────────────┐
         │                                       │
         ▼                                       ▼
┌──────────────────────┐              ┌──────────────────────┐
│  Interpreter Mode    │              │     JIT Mode         │
│  (JIT disabled)      │              │  (JIT enabled)       │
└──────────┬───────────┘              └──────────┬───────────┘
           │                                     │
           ▼                                     ▼
    ┌─────────────┐                      ┌─────────────┐
    │ Start Timer │                      │ Start Timer │
    └──────┬──────┘                      └──────┬──────┘
           │                                     │
           ▼                                     ▼
    ┌─────────────┐                      ┌─────────────┐
    │ Run Query   │                      │ Run Query   │
    │ (N times)   │                      │ (N times)   │
    └──────┬──────┘                      └──────┬──────┘
           │                                     │
           ▼                                     ▼
    ┌─────────────┐                      ┌─────────────┐
    │ Stop Timer  │                      │ Stop Timer  │
    └──────┬──────┘                      └──────┬──────┘
           │                                     │
           └──────────────┬──────────────────────┘
                          │
                          ▼
              ┌───────────────────────┐
              │  Compare Results      │
              │  (row-by-row)         │
              └───────────┬───────────┘
                          │
                          ▼
              ┌───────────────────────┐
              │  Calculate Metrics    │
              │  • Speedup            │
              │  • Results match?     │
              │  • Row count          │
              └───────────┬───────────┘
                          │
                          ▼
              ┌───────────────────────┐
              │  Return               │
              │  BenchmarkResult      │
              └───────────────────────┘
```

---

## Thread Safety Model

```
┌─────────────────────────────────────────────────────────────────┐
│                        Thread Safety                             │
│                                                                  │
│  JITProfiler:                                                   │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │  std::mutex lock                                           │ │
│  │  unordered_map<uint64_t, uint64_t> counts                 │ │
│  │                                                             │ │
│  │  Record(expr, count):                                      │ │
│  │    lock_guard<mutex> guard(lock)                          │ │
│  │    counts[Fingerprint(expr)] += count                     │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  JITCache:                                                      │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │  std::mutex lock                                           │ │
│  │  list<uint64_t> lru_list                                  │ │
│  │  unordered_map<uint64_t, ...> cache                       │ │
│  │                                                             │ │
│  │  Lookup(expr):                                             │ │
│  │    lock_guard<mutex> guard(lock)                          │ │
│  │    auto it = cache.find(Fingerprint(expr))               │ │
│  │    TouchEntry(fp)  // Move to front of LRU               │ │
│  │    return it->function                                     │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  JITDispatcher:                                                 │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │  No mutex needed (reads only)                             │ │
│  │  Configuration changes are rare                            │ │
│  │  Statistics updates are atomic                             │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Thread-Safe Guarantee:                                         │
│  • Multiple threads can call Record() concurrently             │
│  • Multiple threads can call Lookup() concurrently             │
│  • Multiple threads can call TryExecuteJIT() concurrently      │
│  • No data races or deadlocks                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Integration Points

```
┌─────────────────────────────────────────────────────────────────┐
│                    Integration Points                            │
│                                                                  │
│  1. Profiler → Dispatcher                                       │
│     ┌────────────────────────────────────────────────────────┐  │
│     │  uint64_t count = JITProfiler::GetCount(expr)         │  │
│     │  if (count >= threshold) {                             │  │
│     │      TryCompile(expr);                                 │  │
│     │  }                                                      │  │
│     └────────────────────────────────────────────────────────┘  │
│                                                                  │
│  2. Dispatcher → Cache                                          │
│     ┌────────────────────────────────────────────────────────┐  │
│     │  auto func = JITCache::GetInstance().Lookup(expr);    │  │
│     │  if (func) {                                            │  │
│     │      func(inputs, result, count);                      │  │
│     │      return true;  // Used JIT                         │  │
│     │  }                                                      │  │
│     └────────────────────────────────────────────────────────┘  │
│                                                                  │
│  3. Dispatcher → Compiler (Person B implements)                 │
│     ┌────────────────────────────────────────────────────────┐  │
│     │  bool TryCompile(const Expression &expr) {            │  │
│     │      // Generate LLVM IR                               │  │
│     │      auto ir = codegen.GenerateIR(expr);              │  │
│     │                                                         │  │
│     │      // Compile to native                              │  │
│     │      auto func = codegen.CompileToNative(ir);         │  │
│     │                                                         │  │
│     │      // Insert into cache                              │  │
│     │      JITCache::GetInstance().Insert(expr, func, ...); │  │
│     │                                                         │  │
│     │      return true;                                       │  │
│     │  }                                                      │  │
│     └────────────────────────────────────────────────────────┘  │
│                                                                  │
│  4. Benchmark → Dispatcher                                      │
│     ┌────────────────────────────────────────────────────────┐  │
│     │  // Measure interpreter                                │  │
│     │  JITDispatcher::SetEnableJIT(false);                  │  │
│     │  auto time1 = MeasureQueryTime(query);                │  │
│     │                                                         │  │
│     │  // Measure JIT                                        │  │
│     │  JITDispatcher::SetEnableJIT(true);                   │  │
│     │  auto time2 = MeasureQueryTime(query);                │  │
│     │                                                         │  │
│     │  // Calculate speedup                                  │  │
│     │  double speedup = time1 / time2;                       │  │
│     └────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## File Organization

```
duckdb/
├── src/
│   ├── include/duckdb/execution/jit/
│   │   ├── jit_profiler.hpp       ← Part A (existing)
│   │   ├── jit_cache.hpp          ← Part C (new)
│   │   ├── jit_dispatcher.hpp     ← Part C (new)
│   │   └── jit_benchmark.hpp      ← Part C (new)
│   │
│   └── execution/jit/
│       ├── jit_profiler.cpp       ← Part A (existing)
│       ├── jit_cache.cpp          ← Part C (new)
│       ├── jit_dispatcher.cpp     ← Part C (new)
│       └── jit_benchmark.cpp      ← Part C (new)
│
├── test/api/
│   ├── test_jit_profiler.cpp      ← Part A (existing)
│   ├── test_jit_cache.cpp         ← Part C (new)
│   └── test_jit_dispatcher.cpp    ← Part C (new)
│
└── tools/
    ├── jit_profiler_demo.cpp      ← Part A (existing)
    └── jit_benchmark_demo.cpp     ← Part C (new)
```

---

## Summary

This architecture provides:

1. **Clean Separation**: Each part (A, B, C) is independent
2. **Clear Integration**: Well-defined interfaces between parts
3. **Thread Safety**: All components safe for concurrent use
4. **Performance**: O(1) cache operations, minimal overhead
5. **Extensibility**: Easy to add features (persistent cache, etc.)
6. **Testability**: Each component fully unit tested

**Status**: Part C complete, ready for Part B integration.
