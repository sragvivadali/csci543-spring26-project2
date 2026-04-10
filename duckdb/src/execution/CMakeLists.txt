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
  jit/jit_cache.cpp
  jit/jit_dispatcher.cpp
  jit/jit_benchmark.cpp
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

  # Homebrew LLVM is built in shared mode — link against the single shared lib.
  # Using llvm_map_components_to_libnames with individual static .a libs causes
  # unresolved cross-dependencies; the shared libLLVM includes everything.
  set(DUCKDB_LLVM_LIBS LLVM)

  # Store in cache vars so the top-level src/CMakeLists.txt can link them.
  set(DUCKDB_JIT_LLVM_LIBS ${DUCKDB_LLVM_LIBS} CACHE INTERNAL "LLVM libs for JIT")
  set(DUCKDB_JIT_LLVM_LIB_DIRS ${LLVM_LIBRARY_DIRS} CACHE INTERNAL "LLVM lib dirs for JIT")
endif()
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_execution>
    PARENT_SCOPE)
