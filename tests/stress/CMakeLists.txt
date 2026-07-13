# Applies the selected sanitizer (if any) to a target.
function(halcyon_apply_sanitizer target)
    if(HALCYON_SANITIZER)
        target_compile_options(${target} PRIVATE
            -fsanitize=${HALCYON_SANITIZER} -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=${HALCYON_SANITIZER})
    endif()
endfunction()

# ---- Front-end #1: correctness (GoogleTest, CTest label "stress") ----
add_executable(halcyon_stress_tests
    correctness/test_stress_concurrency.cpp
    correctness/test_stress_savepoints.cpp
    support/workload_runner.cpp)
target_link_libraries(halcyon_stress_tests
    PRIVATE halcyon::halcyon GTest::gtest_main Threads::Threads)
target_include_directories(halcyon_stress_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_options(halcyon_stress_tests PRIVATE -Wall -Wextra)
halcyon_apply_sanitizer(halcyon_stress_tests)

include(GoogleTest)
gtest_discover_tests(halcyon_stress_tests PROPERTIES LABELS "stress")

# ---- Front-end #2: perf harness (standalone; not a default CTest test) ----
add_executable(halcyon_stress
    perf/halcyon_stress_main.cpp
    support/workload_runner.cpp)
target_link_libraries(halcyon_stress
    PRIVATE halcyon::halcyon Threads::Threads)
target_include_directories(halcyon_stress
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_options(halcyon_stress PRIVATE -Wall -Wextra)
halcyon_apply_sanitizer(halcyon_stress)

# ---- Front-end #3: batch-insert throughput benchmark (standalone; live Db2) ----
add_executable(halcyon_batch_insert_bench perf/batch_insert_bench.cpp)
target_link_libraries(halcyon_batch_insert_bench PRIVATE halcyon::halcyon)
target_compile_options(halcyon_batch_insert_bench PRIVATE -Wall -Wextra)
halcyon_apply_sanitizer(halcyon_batch_insert_bench)

# ---- Front-end #4: read-throughput (block fetch) benchmark (standalone; live Db2) ----
add_executable(halcyon_select_bench perf/select_bench.cpp)
target_link_libraries(halcyon_select_bench PRIVATE halcyon::halcyon)
target_compile_options(halcyon_select_bench PRIVATE -Wall -Wextra)
halcyon_apply_sanitizer(halcyon_select_bench)
