# Prefer a locally installed GoogleTest; only fetch it as a fallback when no
# suitable installation is found. Set HALCYON_FETCH_GTEST=ON to force fetching.
option(HALCYON_FETCH_GTEST "Always fetch GoogleTest instead of using a local install" OFF)

set(_halcyon_have_local_gtest FALSE)
if(NOT HALCYON_FETCH_GTEST)
    # CONFIG mode first (e.g. Homebrew/vcpkg/system installs that ship a
    # GTestConfig.cmake), then fall back to the FindGTest module.
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        find_package(GTest MODULE QUIET)
    endif()
    if(GTest_FOUND)
        set(_halcyon_have_local_gtest TRUE)
        message(STATUS "Halcyon: using locally installed GoogleTest")
    endif()
endif()

if(NOT _halcyon_have_local_gtest)
    message(STATUS "Halcyon: fetching GoogleTest via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

find_package(Threads REQUIRED)

add_executable(halcyon_unit_tests
    unit/test_version.cpp
    unit/test_error.cpp
    unit/test_result.cpp
    unit/test_cli_seam.cpp
    unit/test_type_binder.cpp
    unit/test_parameters.cpp
    unit/test_result_set.cpp
    unit/test_connection.cpp
    unit/test_statement_cache.cpp
    unit/test_reflect.cpp
    unit/test_sqlstate.cpp
    unit/test_retry.cpp
    unit/test_pool.cpp
    unit/test_pool_maintenance.cpp
    unit/test_async.cpp
    unit/test_transaction.cpp
    unit/test_database.cpp
    unit/test_database_retry.cpp
    unit/test_database_batch.cpp
    unit/test_database_async.cpp
    unit/test_observability.cpp
    unit/test_logging.cpp
    unit/test_savepoint.cpp
    unit/test_lob.cpp
    unit/test_streaming.cpp
    unit/test_lob_write.cpp)

target_link_libraries(halcyon_unit_tests
    PRIVATE halcyon::halcyon GTest::gtest_main Threads::Threads)

target_include_directories(halcyon_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit)

include(GoogleTest)
gtest_discover_tests(halcyon_unit_tests)

# OTel-gated propagation tests: real SpanId parent/child links via the in-memory
# exporter. Built only when the opentelemetry-cpp adapter is enabled.
if(HALCYON_OTEL_ADAPTER_ENABLED)
    add_executable(halcyon_otel_tests unit/test_otel_propagation.cpp)
    target_link_libraries(halcyon_otel_tests
        PRIVATE halcyon::halcyon GTest::gtest_main Threads::Threads
                opentelemetry-cpp::in_memory_span_exporter)
    target_include_directories(halcyon_otel_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit)
    gtest_discover_tests(halcyon_otel_tests)
endif()

if(HALCYON_BUILD_INTEGRATION_TESTS)
    add_subdirectory(integration)
endif()

if(HALCYON_BUILD_STRESS_TESTS)
    add_subdirectory(stress)
endif()
