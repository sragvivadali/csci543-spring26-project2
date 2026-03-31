if(NOT SUN AND BUILD_SHELL)
  add_subdirectory(shell)
endif()

include_directories(${CMAKE_SOURCE_DIR}/src/include)
add_executable(jit_profiler_demo jit_profiler_demo.cpp)
target_link_libraries(jit_profiler_demo duckdb_static ${DUCKDB_EXTRA_LINK_FLAGS})
link_threads(jit_profiler_demo "")
link_extension_libraries(jit_profiler_demo "")
