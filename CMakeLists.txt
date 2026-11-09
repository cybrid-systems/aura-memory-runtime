cmake_minimum_required(VERSION 3.28)
project(aura_memory_runtime CXX)

# Aura's headers use C++26 contracts (pre(...) / contract_assert), so this
# project builds with the same toolchain as Aura CI: GCC 16 in
# ghcr.io/cybrid-systems/dev:v1.0.9.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(AURA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/aura/src)
if(NOT EXISTS ${AURA_SRC}/core/persistent_child_vector.hh)
  message(FATAL_ERROR "third_party/aura missing: run `git submodule update --init --depth 1`")
endif()

# Issue #4946: std/evo-core + std/moead live in Aura, not here. The runtime
# drives them through `aura --serve`; point this at a v0.2-era aura binary.
set(AMR_AURA_BIN "" CACHE FILEPATH "Path to an aura binary with std/moead")
if(NOT AMR_AURA_BIN)
  find_program(AMR_AURA_BIN NAMES aura)
endif()

add_library(amr STATIC
  src/ast.cpp
  src/rules.cpp
  src/store.cpp
  src/plan.cpp
  src/log.cpp
  src/fiber.cpp
  src/runtime.cpp
  src/evo.cpp
  # Reused verbatim from Aura: per-fiber execution context (Issue #4716).
  ${AURA_SRC}/core/fiber_context.cpp
  # Reused verbatim from Aura: C++26 contract violation handler (needed by
  # every TU that includes Aura headers with pre()/contract_assert).
  ${AURA_SRC}/core/contract_handler.cpp
)
set_source_files_properties(${AURA_SRC}/core/contract_handler.cpp
  PROPERTIES COMPILE_FLAGS "-include contracts")
target_include_directories(amr PUBLIC include ${AURA_SRC})
target_compile_options(amr PUBLIC -fcontracts -Wall -Wextra)

enable_testing()
foreach(t test_main_line_isolation test_abort_no_trace test_torn_writes
          test_hot_state_replay test_cow_sharing test_promote_improves
          test_rules_engine test_tiers_and_limits test_soak_worldlines
          test_evo_gate)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE amr)
  add_test(NAME ${t} COMMAND ${t})
  set_tests_properties(${t} PROPERTIES TIMEOUT 300)
endforeach()

# Needs an aura binary (AMR_AURA_BIN / aura in PATH). Without one the test
# prints SKIP and passes; CI that wants the coverage must provide v0.2 aura.
add_executable(test_evo_moead tests/test_evo_moead.cpp)
target_link_libraries(test_evo_moead PRIVATE amr)
add_test(NAME test_evo_moead COMMAND test_evo_moead)
set_tests_properties(test_evo_moead PROPERTIES TIMEOUT 300 LABELS "evo;requires_aura")
if(AMR_AURA_BIN)
  set_tests_properties(test_evo_moead PROPERTIES
    ENVIRONMENT "AMR_AURA_BIN=${AMR_AURA_BIN}")
endif()

add_executable(amr_bench bench/bench.cpp)
target_link_libraries(amr_bench PRIVATE amr)
