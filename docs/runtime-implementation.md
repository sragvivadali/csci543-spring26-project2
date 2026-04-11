# JIT Runtime System Implementation (Part C)

## Overview

This document describes the implementation of Part C: Runtime / Cache / Evaluation features for the DuckDB JIT system. This component provides the execution infrastructure that integrates with the profiler (Part A) and LLVM compiler (Part B).

---

## Architecture

The runtime system consists of three main subsystems:

### 1. JITCache - Compiled Function Cache

**Purpose**: Store and manage compiled JIT functions with LRU eviction.

**Key Features**:
- Fingerprint-based lookup (uses same fingerprinting as JITProfiler)
- LRU (Least Recently Used) eviction policy
- Thread-safe operations
- Configurable cache size
- Hit/miss statistics tracking

**Files**:
- `src/include/duckdb/execution/jit/jit_cache.hpp`
- `src/execution/jit/jit_cache.cpp`

**API**:
```cpp
auto &cache = JITCache::GetInstance();

// Lookup compiled function
JITCompiledFunction func = cache.Lookup(expr);

// Insert compiled function
cache.Insert(expr, compiled_func, compile_time_us);

// Configuration
cache.SetMaxEntries(1000);
cache.Clear();

// Statistics
auto stats = cache.GetStats();
cache.PrintCache();
```

### 2. JITDispatcher - Execution Path Dispatcher

**Purpose**: Decide whether to use JIT-compiled code or fall back to interpreter.

**Key Features**:
- Checks cache for compiled functions
- Uses profiling data to identify hot expressions
- Triggers compilation when threshold is exceeded
- Falls back to interpreter when needed
- Tracks execution statistics

**Files**:
- `src/include/duckdb/execution/jit/jit_dispatcher.hpp`
- `src/execution/jit/jit_dispatcher.cpp`

**Decision Logic**:
1. Check if JIT is enabled
2. Lookup expression in cache
3. If found, execute compiled code
4. If not found, check if expression is "hot" (execution count > threshold)
5. If hot, trigger compilation and retry
6. Otherwise, fall back to interpreter

**API**:
```cpp
auto &dispatcher = JITDispatcher::GetInstance();

// Configuration
dispatcher.SetEnableJIT(true);
dispatcher.SetCompilationThreshold(10000);

// Execution (called from ExpressionExecutor)
bool used_jit = dispatcher.TryExecuteJIT(expr, executor, inputs, result, count);

// Statistics
auto stats = dispatcher.GetStats();
dispatcher.PrintStats();
```

### 3. JITBenchmark - Performance Measurement Harness

**Purpose**: Measure and validate JIT performance vs interpreter.

**Key Features**:
- Run queries with/without JIT
- Compare execution times
- Validate result correctness
- Generate performance reports
- Export results to CSV

**Files**:
- `src/include/duckdb/execution/jit/jit_benchmark.hpp`
- `src/execution/jit/jit_benchmark.cpp`

**API**:
```cpp
JITBenchmark bench;

// Run single query
auto result = bench.RunQuery("SELECT a + b FROM test");

// Run benchmark suite
vector<string> queries = {...};
bench.RunBenchmarkSuite(queries, iterations);

// Generate reports
bench.PrintReport();
bench.ExportCSV("results.csv");
```

---

## Integration Points

### With Part A (Profiler)

The runtime system uses the profiler to make intelligent decisions:

```cpp
// Check if expression is hot enough to compile
uint64_t count = JITProfiler::GetInstance().GetCount(expr);
if (count >= compilation_threshold) {
    TryCompile(expr);
}
```

### With Part B (LLVM Compiler)

The dispatcher provides a hook for the compiler:

```cpp
bool JITDispatcher::TryCompile(const Expression &expr) {
    compilation_attempts++;
    
    // TODO: Person B implements this
    // 1. Generate LLVM IR for expression
    // 2. Compile to native code
    // 3. Insert into cache:
    //    JITCache::GetInstance().Insert(expr, compiled_func, compile_time);
    // 4. Return true on success
    
    return false; // Stub for now
}
```

### Integration Flow

```
Query Execution
    ↓
ExpressionExecutor::Execute()
    ↓
JITProfiler::Record() ← Track execution count
    ↓
JITDispatcher::TryExecuteJIT()
    ↓
    ├─→ JITCache::Lookup() ← Check for compiled code
    │       ↓
    │   Found? → Execute compiled code
    │       ↓
    │   Not found? → Check if hot
    │       ↓
    │   Hot? → TryCompile() ← Person B's LLVM compiler
    │       ↓
    │   Insert into cache
    │       ↓
    └─→ Fall back to interpreter
```

---

## Cache Eviction Policy

The cache uses an LRU (Least Recently Used) policy:

1. Each cache entry tracks its position in an LRU list
2. On lookup, entry is moved to front of list (most recently used)
3. On insertion, new entry is added to front
4. When cache is full, entry at back of list is evicted

**Implementation**:
```cpp
// LRU list: most recent at front
std::list<uint64_t> lru_list;

// Cache: fingerprint -> (entry, iterator into lru_list)
unordered_map<uint64_t, pair<JITCacheEntry, list<uint64_t>::iterator>> cache;

void TouchEntry(uint64_t fp) {
    // Move to front of LRU list
    lru_list.erase(entry_iterator);
    lru_list.push_front(fp);
    entry_iterator = lru_list.begin();
}

void EvictLRU() {
    uint64_t evict_fp = lru_list.back();
    lru_list.pop_back();
    cache.erase(evict_fp);
}
```

---

## Configuration Options

### JITDispatcher Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `enable_jit` | `false` | Master switch to enable/disable JIT |
| `compilation_threshold` | `10000` | Min tuple count before compiling expression |

### JITCache Configuration

| Option | Default | Description |
|--------|---------|-------------|
| `max_entries` | `1000` | Maximum number of cached functions |

---

## Statistics and Monitoring

### Cache Statistics

```cpp
struct CacheStats {
    idx_t total_entries;      // Current cache size
    idx_t total_hits;         // Successful lookups
    idx_t total_misses;       // Failed lookups
    idx_t total_evictions;    // LRU evictions
    idx_t max_entries;        // Cache capacity
};
```

### Dispatcher Statistics

```cpp
struct DispatchStats {
    idx_t jit_executions;         // Times JIT code was used
    idx_t interpreter_executions; // Times interpreter was used
    idx_t compilation_attempts;   // Compilation tries
    idx_t compilation_successes;  // Successful compilations
    idx_t compilation_failures;   // Failed compilations
};
```

### Benchmark Results

```cpp
struct BenchmarkResult {
    string query;
    double interpreter_time_ms;
    double jit_time_ms;
    double speedup;           // interpreter_time / jit_time
    bool results_match;       // Correctness validation
    idx_t row_count;
    idx_t compilation_count;
};
```

---

## Testing

### Unit Tests

Three test files provide comprehensive coverage:

1. **test_jit_cache.cpp**
   - Basic insert/lookup
   - LRU eviction
   - LRU ordering with access
   - Clear functionality
   - Duplicate insert handling
   - Dynamic max_entries adjustment

2. **test_jit_dispatcher.cpp**
   - Configuration management
   - Statistics tracking
   - Threshold logic
   - Enable/disable behavior

3. **test_jit_profiler.cpp** (existing)
   - Fingerprint consistency
   - Expression recording
   - Count aggregation

### Running Tests

```bash
cd build
cmake --build . --target unittest -j 8
./test/unittest "[jit_cache]"
./test/unittest "[jit_dispatcher]"
./test/unittest "[jit_profiler]"
```

---

## Demo Tool

A comprehensive demo tool showcases all runtime features:

**File**: `tools/jit_benchmark_demo.cpp`

**Build**:
```bash
cd build
cmake --build . --target jit_benchmark_demo
```

**Run**:
```bash
./tools/jit_benchmark_demo
```

**Demo Sections**:
1. Basic setup and configuration
2. Expression profiler demo
3. JIT cache demo
4. JIT dispatcher demo
5. Benchmark harness demo
6. Full system integration overview

---

## Benchmark Harness Usage

### Example: Simple Benchmark

```cpp
#include "duckdb/execution/jit/jit_benchmark.hpp"

DuckDB db(nullptr);
JITBenchmark bench(db);

// Run single query
auto result = bench.RunQuery("SELECT a + b FROM test WHERE a > 100");

cout << "Interpreter: " << result.interpreter_time_ms << "ms" << endl;
cout << "JIT: " << result.jit_time_ms << "ms" << endl;
cout << "Speedup: " << result.speedup << "x" << endl;
```

### Example: Benchmark Suite

```cpp
vector<string> queries = {
    "SELECT x + y FROM data",
    "SELECT x * y FROM data WHERE x > 1000",
    "SELECT x + y + z FROM data"
};

JITBenchmark bench;
bench.RunBenchmarkSuite(queries, 5); // 5 iterations each
bench.PrintReport();
bench.ExportCSV("results.csv");
```

### CSV Output Format

```csv
Query,Interpreter_ms,JIT_ms,Speedup,Results_Match,Row_Count,Compilation_Count
"SELECT x + y FROM data",45.23,12.34,3.67,true,100000,1
"SELECT x * y FROM data WHERE x > 1000",38.91,10.22,3.81,true,50000,1
```

---

## Result Validation

The benchmark harness validates that JIT produces identical results to the interpreter:

```cpp
bool ValidateResults(Connection &conn1, Connection &conn2, const string &query) {
    // Run with interpreter
    SetEnableJIT(false);
    auto result1 = conn1.Query(query);
    
    // Run with JIT
    SetEnableJIT(true);
    auto result2 = conn2.Query(query);
    
    // Compare results
    return ResultsToString(result1) == ResultsToString(result2);
}
```

Any mismatch is reported in the benchmark results.

---

## Performance Reporting

### Console Report Format

```
=== JIT Benchmark Report ===
================================================================================

Summary:
  Total queries: 4
  Total rows processed: 300000
  Result mismatches: 0

  Total interpreter time: 156.42ms
  Total JIT time: 42.18ms
  Overall speedup: 3.71x

--------------------------------------------------------------------------------
Query                                    Interp(ms)    JIT(ms)   Speedup  Match
--------------------------------------------------------------------------------
SELECT x + y FROM data                        45.23      12.34      3.67    YES
SELECT x * y FROM data WHERE x > 1000         38.91      10.22      3.81    YES
SELECT x + y + z FROM data                    52.14      13.89      3.75    YES
SELECT x * 2 + y * 3 FROM data                20.14       5.73      3.52    YES
================================================================================
```

---

## Future Enhancements

### Potential Improvements

1. **Cache Invalidation**
   - Invalidate on schema changes
   - Invalidate on statistics updates
   - Version-based invalidation

2. **Advanced Eviction Policies**
   - LFU (Least Frequently Used)
   - Size-aware eviction (large functions evicted first)
   - Cost-based eviction (compilation time vs hit count)

3. **Adaptive Compilation Threshold**
   - Adjust threshold based on compilation success rate
   - Per-expression-type thresholds
   - Query-specific thresholds

4. **Persistent Cache**
   - Save compiled functions to disk
   - Load on startup
   - Cross-session cache reuse

5. **Parallel Compilation**
   - Background compilation thread
   - Non-blocking compilation
   - Compilation queue

---

## Integration with Person B's Compiler

When Person B completes the LLVM compiler, integration is straightforward:

### Step 1: Implement TryCompile

```cpp
bool JITDispatcher::TryCompile(const Expression &expr) {
    compilation_attempts++;
    
    try {
        // Generate LLVM IR
        LLVMCodeGenerator codegen;
        auto ir_module = codegen.GenerateIR(expr);
        
        // Compile to native code
        auto compiled_func = codegen.CompileToNative(ir_module);
        
        // Measure compilation time
        Timer timer;
        // ... compilation happens ...
        uint64_t compile_time_us = timer.ElapsedMicroseconds();
        
        // Insert into cache
        JITCache::GetInstance().Insert(expr, compiled_func, compile_time_us);
        
        compilation_successes++;
        return true;
        
    } catch (std::exception &e) {
        compilation_failures++;
        return false;
    }
}
```

### Step 2: Update ExpressionExecutor

```cpp
void ExpressionExecutor::Execute(const Expression &expr, DataChunk &input, Vector &result) {
    // ... existing code ...
    
    // Try JIT execution
    Vector *inputs[MAX_INPUTS];
    // ... prepare inputs ...
    
    bool used_jit = JITDispatcher::GetInstance().TryExecuteJIT(
        expr, *this, inputs, result, count);
    
    if (used_jit) {
        return; // JIT handled it
    }
    
    // Fall back to interpreter
    // ... existing interpreter code ...
}
```

---

## Summary

The runtime system (Part C) provides:

✅ **JITCache**: Thread-safe LRU cache for compiled functions  
✅ **JITDispatcher**: Intelligent JIT vs interpreter selection  
✅ **JITBenchmark**: Comprehensive performance measurement and validation  
✅ **Unit Tests**: Full coverage of cache and dispatcher functionality  
✅ **Demo Tool**: Showcases all runtime features  
✅ **Documentation**: Complete API and integration guide  

**Status**: Ready for integration with LLVM compiler (Part B)

**Next Steps**:
1. Person B implements LLVM code generation
2. Person B implements `TryCompile()` function
3. Integrate dispatcher into ExpressionExecutor
4. Run benchmarks to measure real speedup
5. Tune cache size and compilation thresholds
