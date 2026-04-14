# Architecture

## Execution Path

1. Expression batch arrives in `ExpressionExecutor`.
2. `JITProfiler::Record(expr, count)` updates hotness counters.
3. `JITDispatcher::TryExecuteJIT(...)` attempts:
   - Cache lookup, then execute on hit.
   - Compile on hot miss (`ShouldCompile` + `TryCompile`).
   - Interpreter fallback when not compiled.
4. Dispatcher stats track JIT/interpreter usage and compilation outcomes.

## End-to-End Flow Diagram

```text
┌─────────────────────────────────────────────────────────────┐
│                  ExpressionExecutor::Execute                │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               ▼
                   ┌───────────────────────────┐
                   │ JITProfiler::Record       │
                   │ (fingerprint + hotness)   │
                   └──────────────┬────────────┘
                                  │
                                  ▼
                  ┌─────────────────────────────────┐
                  │ JITDispatcher::TryExecuteJIT    │
                  └──────────────┬──────────────────┘
                                 │
                     ┌───────────┴───────────┐
                     │                       │
                     ▼                       ▼
          ┌────────────────────┐   ┌──────────────────────┐
          │ Cache hit?         │   │ Cache miss           │
          │ JITCache::Lookup   │   │ ShouldCompile(expr)? │
          └─────────┬──────────┘   └──────────┬───────────┘
                    │                         │
            yes     ▼                         ▼ no
         ┌──────────────────┐          ┌───────────────┐
         │ Execute JIT fn   │          │ Interpreter   │
         └──────────────────┘          └───────────────┘
                                              ▲
                                              │
                                      yes     │
                                       ▼      │
                              ┌────────────────────────┐
                              │ TryCompile + Insert    │
                              │ into JITCache          │
                              └──────────┬─────────────┘
                                         │ success
                                         ▼
                                  ┌──────────────┐
                                  │ Execute JIT  │
                                  └──────────────┘
```

## Dataflow

- **Key**: expression fingerprint (`uint64_t`) from `JITProfiler::Fingerprint`.
- **Compile product**: `JITCompiledFunction` wrapper stored in `JITCache`.
- **Runtime input**: sparse vector pointer array indexed by source column id.

## Component Interaction Diagram

```text
           fingerprint + hotness
┌────────────────────┐   count / threshold   ┌────────────────────┐
│    JITProfiler     │ ────────────────────► │   JITDispatcher    │
└────────────────────┘                       └─────────┬──────────┘
                                                       │
                                            lookup/insert by fingerprint
                                                       │
                                              ┌────────▼────────┐
                                              │    JITCache     │
                                              └────────┬────────┘
                                                       │
                                          compile miss │
                                                       ▼
                                              ┌──────────────────┐
                                              │   JITCompiler    │
                                              │ CanCompile/Compile│
                                              └──────────────────┘
```

## Operational Controls

- `SetEnableJIT(bool)`: master runtime switch.
- `SetCompilationThreshold(idx_t)`: hotness threshold before compile attempts.
- `JITCache::SetMaxEntries(idx_t)`: cache capacity policy.

## Safety and Fallback Model

- Dispatcher never blocks query execution on compilation failure.
- Unsupported trees and compilation failures increment failure stats and continue in interpreter mode.
- JIT behavior is observable via `JITDispatcher::PrintStats()`.
