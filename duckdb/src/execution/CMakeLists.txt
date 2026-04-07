add_subdirectory(expression_executor)
add_subdirectory(nested_loop_join)
add_subdirectory(operator)
add_subdirectory(physical_plan)
add_subdirectory(index)
add_subdirectory(sample)

add_library_unity(
  duckdb_execution
  OBJECT
  adaptive_filter.cpp
  aggregate_hashtable.cpp
  base_aggregate_hashtable.cpp
  column_binding_resolver.cpp
  expression_executor.cpp
  expression_executor_state.cpp
  jit/jit_profiler.cpp
  jit/jit_compiler.cpp
  join_hashtable.cpp
  perfect_aggregate_hashtable.cpp
  physical_operator.cpp
  physical_plan_generator.cpp
  radix_partitioned_hashtable.cpp)

# ── LLVM JIT support (Person B) ──────────────────────────────────────────────
# Enable with: cmake -DDUCKDB_JIT_ENABLED=ON ...
# Requires LLVM 14+ installed. On Ubuntu: sudo apt install llvm-14-dev
# On macOS with Homebrew: brew install llvm
option(DUCKDB_JIT_ENABLED "Enable LLVM JIT compilation for hot expressions" OFF)

if(DUCKDB_JIT_ENABLED)
  find_package(LLVM REQUIRED CONFIG)
  message(STATUS "LLVM JIT enabled: LLVM ${LLVM_VERSION} found at ${LLVM_DIR}")
  include_directories(${LLVM_INCLUDE_DIRS})
  add_definitions(${LLVM_DEFINITIONS} -DDUCKDB_JIT_ENABLED)

  # Map required LLVM components to library names.
  # core + orcjit + native covers: IR builder, ORC/LLJIT, and native target support.
  llvm_map_components_to_libnames(DUCKDB_LLVM_LIBS
    core
    orcjit
    native        # InitializeNativeTarget + InitializeNativeTargetAsmPrinter
    support
  )

  # Link LLVM into the main duckdb target (defined later in the top-level CMake).
  # We store the libs in a cache var so the top-level CMakeLists can use them.
  set(DUCKDB_JIT_LLVM_LIBS ${DUCKDB_LLVM_LIBS} CACHE INTERNAL "LLVM libs for JIT")
endif()
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_execution>
    PARENT_SCOPE)
