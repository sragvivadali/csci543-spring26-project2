# JIT Runtime System - Usage Examples

This document provides practical examples of how to use the JIT runtime system.

---

## Example 1: Basic Cache Usage

```cpp
#include "duckdb/execution/jit/jit_cache.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"

using namespace duckdb;

// Define a compiled function (stub for demonstration)
void MyCompiledFunction(Vector **inputs, Vector &result, idx_t count) {
    // Compiled code would go here
    // For now, just a placeholder
}

int main() {
    auto &cache = JITCache::GetInstance();
    
    // Create an expression
    auto expr = make_uniq<BoundComparisonExpression>(
        ExpressionType::COMPARE_GREATERTHAN,
        make_uniq<BoundConstantExpression>(Value::INTEGER(10)),
        make_uniq<BoundConstantExpression>(Value::INTEGER(5))
    );
    
    // Insert into cache
    cache.Insert(*expr, MyCompiledFunction, 1500); // 1500us compile time
    
    // Lookup
    auto func = cache.Lookup(*expr);
    if (func) {
        std::cout << "Found in cache!" << std::endl;
        // func(inputs, result, count);  // Execute
    }
    
    // Print stats
    cache.PrintCache();
    
    return 0;
}
```

---

## Example 2: Dispatcher Configuration

```cpp
#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"

using namespace duckdb;

int main() {
    auto &dispatcher = JITDispatcher::GetInstance();
    auto &profiler = JITProfiler::GetInstance();
    
    // Configure JIT system
    dispatcher.SetEnableJIT(true);
    dispatcher.SetCompilationThreshold(5000);  // Compile after 5000 tuples
    
    // Simulate some expression executions
    auto expr = make_uniq<BoundComparisonExpression>(
        ExpressionType::COMPARE_EQUAL,
        make_uniq<BoundConstantExpression>(Value::INTEGER(42)),
        make_uniq<BoundConstantExpression>(Value::INTEGER(42))
    );
    
    // Record executions (simulating query processing)
    profiler.Record(*expr, 1000);
    profiler.Record(*expr, 2000);
    profiler.Record(*expr, 3000);  // Total: 6000, above threshold
    
    // Check if should compile
    uint64_t count = profiler.GetCount(*expr);
    std::cout << "Expression executed " << count << " times" << std::endl;
    
    if (count >= dispatcher.GetCompilationThreshold()) {
        std::cout << "Expression is hot! Should compile." << std::endl;
    }
    
    // Print dispatcher stats
    dispatcher.PrintStats();
    
    return 0;
}
```

---

## Example 3: Running Benchmarks

```cpp
#include "duckdb/execution/jit/jit_benchmark.hpp"
#include "duckdb/main/database.hpp"

using namespace duckdb;

int main() {
    // Create database
    DuckDB db(nullptr);
    Connection conn(db);
    
    // Setup test data
    conn.Query("CREATE TABLE test (a INTEGER, b INTEGER, c DOUBLE)");
    conn.Query("INSERT INTO test SELECT i, i*2, i*1.5 FROM range(100000) t(i)");
    
    // Create benchmark
    JITBenchmark bench(db);
    
    // Run single query
    auto result = bench.RunQuery("SELECT a + b FROM test WHERE a > 1000", 5);
    
    std::cout << "Query: " << result.query << std::endl;
    std::cout << "Interpreter time: " << result.interpreter_time_ms << "ms" << std::endl;
    std::cout << "JIT time: " << result.jit_time_ms << "ms" << std::endl;
    std::cout << "Speedup: " << result.speedup << "x" << std::endl;
    std::cout << "Results match: " << (result.results_match ? "YES" : "NO") << std::endl;
    
    return 0;
}
```

---

## Example 4: Benchmark Suite

```cpp
#include "duckdb/execution/jit/jit_benchmark.hpp"
#include "duckdb/main/database.hpp"

using namespace duckdb;

int main() {
    DuckDB db(nullptr);
    Connection conn(db);
    
    // Setup
    conn.Query("CREATE TABLE data (x INTEGER, y INTEGER, z DOUBLE)");
    conn.Query("INSERT INTO data SELECT i, i*2, i*1.5 FROM range(100000) t(i)");
    
    // Define benchmark queries
    vector<string> queries = {
        "SELECT x + y FROM data",
        "SELECT x * 2 + y * 3 FROM data",
        "SELECT x + y + z FROM data WHERE x > 1000",
        "SELECT x, y, x+y AS sum FROM data WHERE z > 500.0"
    };
    
    // Run benchmark suite
    JITBenchmark bench(db);
    bench.RunBenchmarkSuite(queries, 3);  // 3 iterations each
    
    // Print report
    bench.PrintReport();
    
    // Export to CSV
    bench.ExportCSV("benchmark_results.csv");
    
    return 0;
}
```

---

## Example 5: Integration with Person B's Compiler

```cpp
// This is what Person B needs to implement

#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_cache.hpp"
#include "duckdb/execution/jit/jit_benchmark.hpp"

// Hypothetical LLVM code generator (Person B's work)
class LLVMCodeGenerator {
public:
    // Generate LLVM IR for an expression
    llvm::Module* GenerateIR(const Expression &expr) {
        // Person B implements this
        // Generate IR for arithmetic, comparison, etc.
        return nullptr;  // Placeholder
    }
    
    // Compile IR to native function
    JITCompiledFunction CompileToNative(llvm::Module *module) {
        // Person B implements this
        // Use LLVM JIT to compile to native code
        return nullptr;  // Placeholder
    }
};

// Integration point in jit_dispatcher.cpp
bool JITDispatcher::TryCompile(const Expression &expr) {
    compilation_attempts++;
    
    try {
        // 1. Generate LLVM IR
        LLVMCodeGenerator codegen;
        auto ir_module = codegen.GenerateIR(expr);
        
        if (!ir_module) {
            compilation_failures++;
            return false;
        }
        
        // 2. Compile to native code
        Timer timer;
        auto compiled_func = codegen.CompileToNative(ir_module);
        uint64_t compile_time_us = timer.ElapsedMicroseconds();
        
        if (!compiled_func) {
            compilation_failures++;
            return false;
        }
        
        // 3. Insert into cache
        JITCache::GetInstance().Insert(expr, compiled_func, compile_time_us);
        
        compilation_successes++;
        return true;
        
    } catch (std::exception &e) {
        std::cerr << "Compilation failed: " << e.what() << std::endl;
        compilation_failures++;
        return false;
    }
}
```

---

## Example 6: Monitoring and Debugging

```cpp
#include "duckdb/execution/jit/jit_cache.hpp"
#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"

using namespace duckdb;

void PrintSystemStatus() {
    std::cout << "=== JIT System Status ===" << std::endl;
    
    // Dispatcher status
    auto &dispatcher = JITDispatcher::GetInstance();
    std::cout << "\nDispatcher:" << std::endl;
    std::cout << "  JIT Enabled: " << (dispatcher.IsJITEnabled() ? "YES" : "NO") << std::endl;
    std::cout << "  Threshold: " << dispatcher.GetCompilationThreshold() << std::endl;
    
    auto dispatch_stats = dispatcher.GetStats();
    std::cout << "  JIT executions: " << dispatch_stats.jit_executions << std::endl;
    std::cout << "  Interpreter executions: " << dispatch_stats.interpreter_executions << std::endl;
    std::cout << "  Compilation attempts: " << dispatch_stats.compilation_attempts << std::endl;
    std::cout << "  Compilation successes: " << dispatch_stats.compilation_successes << std::endl;
    
    // Cache status
    auto &cache = JITCache::GetInstance();
    auto cache_stats = cache.GetStats();
    std::cout << "\nCache:" << std::endl;
    std::cout << "  Entries: " << cache_stats.total_entries 
              << "/" << cache_stats.max_entries << std::endl;
    std::cout << "  Hits: " << cache_stats.total_hits << std::endl;
    std::cout << "  Misses: " << cache_stats.total_misses << std::endl;
    std::cout << "  Evictions: " << cache_stats.total_evictions << std::endl;
    
    if (cache_stats.total_hits + cache_stats.total_misses > 0) {
        double hit_rate = (100.0 * cache_stats.total_hits) / 
                         (cache_stats.total_hits + cache_stats.total_misses);
        std::cout << "  Hit rate: " << hit_rate << "%" << std::endl;
    }
    
    // Profiler status
    std::cout << "\nProfiler:" << std::endl;
    JITProfiler::GetInstance().PrintStats();
}

int main() {
    // Run some queries...
    
    // Print status
    PrintSystemStatus();
    
    return 0;
}
```

---

## Example 7: Testing Cache Eviction

```cpp
#include "duckdb/execution/jit/jit_cache.hpp"

using namespace duckdb;

void DummyFunc(Vector **inputs, Vector &result, idx_t count) {
    // Placeholder
}

int main() {
    auto &cache = JITCache::GetInstance();
    cache.Clear();
    cache.SetMaxEntries(5);  // Small cache for testing
    
    // Create 10 different expressions
    vector<unique_ptr<Expression>> expressions;
    for (int i = 0; i < 10; i++) {
        expressions.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(i)));
    }
    
    // Insert all 10 (will trigger evictions)
    for (int i = 0; i < 10; i++) {
        std::cout << "Inserting expression " << i << std::endl;
        cache.Insert(*expressions[i], DummyFunc, 1000);
        
        auto stats = cache.GetStats();
        std::cout << "  Cache size: " << stats.total_entries << std::endl;
        std::cout << "  Evictions: " << stats.total_evictions << std::endl;
    }
    
    // Check which ones are still cached
    std::cout << "\nChecking cache contents:" << std::endl;
    for (int i = 0; i < 10; i++) {
        auto func = cache.Lookup(*expressions[i]);
        std::cout << "Expression " << i << ": " 
                  << (func ? "CACHED" : "EVICTED") << std::endl;
    }
    
    // Print final cache state
    cache.PrintCache();
    
    return 0;
}
```

---

## Example 8: Performance Comparison

```cpp
#include "duckdb/execution/jit/jit_benchmark.hpp"
#include "duckdb/main/database.hpp"

using namespace duckdb;

int main() {
    DuckDB db(nullptr);
    Connection conn(db);
    
    // Create large dataset
    conn.Query("CREATE TABLE large_data (a INTEGER, b INTEGER)");
    conn.Query("INSERT INTO large_data SELECT i, i*2 FROM range(1000000) t(i)");
    
    // Test different query types
    vector<string> query_types = {
        "Simple addition",
        "Complex arithmetic",
        "Filtered addition",
        "Multiple operations"
    };
    
    vector<string> queries = {
        "SELECT a + b FROM large_data",
        "SELECT a * 2 + b * 3 - a / 2 FROM large_data",
        "SELECT a + b FROM large_data WHERE a > 500000",
        "SELECT a + b, a * b, a - b FROM large_data"
    };
    
    JITBenchmark bench(db);
    
    for (size_t i = 0; i < queries.size(); i++) {
        std::cout << "\n=== " << query_types[i] << " ===" << std::endl;
        
        auto result = bench.RunQuery(queries[i], 5);
        
        std::cout << "Interpreter: " << result.interpreter_time_ms << "ms" << std::endl;
        std::cout << "JIT: " << result.jit_time_ms << "ms" << std::endl;
        std::cout << "Speedup: " << result.speedup << "x" << std::endl;
        
        if (result.speedup > 1.5) {
            std::cout << "✅ Good speedup!" << std::endl;
        } else if (result.speedup > 1.0) {
            std::cout << "⚠️  Modest speedup" << std::endl;
        } else {
            std::cout << "❌ No speedup (JIT overhead too high)" << std::endl;
        }
    }
    
    return 0;
}
```

---

## Example 9: Adaptive Threshold Testing

```cpp
#include "duckdb/execution/jit/jit_dispatcher.hpp"
#include "duckdb/execution/jit/jit_profiler.hpp"
#include "duckdb/main/database.hpp"

using namespace duckdb;

int main() {
    DuckDB db(nullptr);
    Connection conn(db);
    
    conn.Query("CREATE TABLE test (x INTEGER)");
    conn.Query("INSERT INTO test SELECT i FROM range(100000) t(i)");
    
    // Test different thresholds
    vector<idx_t> thresholds = {1000, 5000, 10000, 50000};
    
    for (auto threshold : thresholds) {
        std::cout << "\n=== Testing threshold: " << threshold << " ===" << std::endl;
        
        // Configure
        auto &dispatcher = JITDispatcher::GetInstance();
        dispatcher.SetEnableJIT(true);
        dispatcher.SetCompilationThreshold(threshold);
        dispatcher.ResetStats();
        
        JITProfiler::GetInstance().Reset();
        
        // Run query multiple times
        for (int i = 0; i < 5; i++) {
            conn.Query("SELECT x * 2 FROM test");
        }
        
        // Print stats
        auto stats = dispatcher.GetStats();
        std::cout << "Compilation attempts: " << stats.compilation_attempts << std::endl;
        std::cout << "JIT executions: " << stats.jit_executions << std::endl;
        std::cout << "Interpreter executions: " << stats.interpreter_executions << std::endl;
    }
    
    return 0;
}
```

---

## Example 10: CSV Export and Analysis

```cpp
#include "duckdb/execution/jit/jit_benchmark.hpp"
#include "duckdb/main/database.hpp"
#include <fstream>

using namespace duckdb;

int main() {
    DuckDB db(nullptr);
    Connection conn(db);
    
    // Setup
    conn.Query("CREATE TABLE data (a INTEGER, b INTEGER)");
    conn.Query("INSERT INTO data SELECT i, i*2 FROM range(100000) t(i)");
    
    // Run benchmarks
    vector<string> queries = {
        "SELECT a + b FROM data",
        "SELECT a * b FROM data",
        "SELECT a - b FROM data",
        "SELECT a + b + a * b FROM data"
    };
    
    JITBenchmark bench(db);
    bench.RunBenchmarkSuite(queries, 5);
    
    // Export to CSV
    bench.ExportCSV("results.csv");
    
    // Read and analyze CSV (demonstration)
    std::ifstream file("results.csv");
    std::string line;
    
    std::cout << "\nCSV Contents:" << std::endl;
    while (std::getline(file, line)) {
        std::cout << line << std::endl;
    }
    
    // You can now import this CSV into Excel, Python, R, etc. for analysis
    std::cout << "\nResults exported to results.csv" << std::endl;
    std::cout << "Import into Excel/Python for further analysis" << std::endl;
    
    return 0;
}
```

---

## Common Patterns

### Pattern 1: Enable JIT for Production

```cpp
// At application startup
void InitializeJIT() {
    auto &dispatcher = JITDispatcher::GetInstance();
    dispatcher.SetEnableJIT(true);
    dispatcher.SetCompilationThreshold(10000);
    
    auto &cache = JITCache::GetInstance();
    cache.SetMaxEntries(1000);
}
```

### Pattern 2: Disable JIT for Debugging

```cpp
// When debugging expression evaluation
void DisableJITForDebugging() {
    JITDispatcher::GetInstance().SetEnableJIT(false);
}
```

### Pattern 3: Warm Up Cache

```cpp
// Pre-compile common expressions
void WarmUpCache() {
    // Run common queries once to trigger compilation
    Connection conn(db);
    conn.Query("SELECT a + b FROM common_table");
    conn.Query("SELECT a * b FROM common_table");
    // etc.
}
```

### Pattern 4: Monitor Performance

```cpp
// Periodically check JIT effectiveness
void MonitorJIT() {
    auto stats = JITDispatcher::GetInstance().GetStats();
    
    if (stats.jit_executions + stats.interpreter_executions > 0) {
        double jit_ratio = (100.0 * stats.jit_executions) / 
                          (stats.jit_executions + stats.interpreter_executions);
        
        if (jit_ratio < 10.0) {
            std::cout << "Warning: Low JIT usage (" << jit_ratio << "%)" << std::endl;
            std::cout << "Consider lowering compilation threshold" << std::endl;
        }
    }
}
```

---

## Compilation

To compile any of these examples:

```bash
g++ -std=c++17 example.cpp \
    -I/path/to/duckdb/src/include \
    -L/path/to/duckdb/build \
    -lduckdb_static \
    -lpthread \
    -o example
```

Or use CMake:

```cmake
add_executable(my_example example.cpp)
target_link_libraries(my_example duckdb_static)
```

---

## Next Steps

1. Review these examples
2. Try running the demo tool
3. Experiment with different configurations
4. Integrate with Person B's LLVM compiler
5. Run benchmarks on real queries
6. Tune parameters for your workload
