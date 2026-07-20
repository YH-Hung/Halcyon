add_executable(halcyon_integration_tests
    test_db2_roundtrip.cpp
    test_db2_savepoints_isolation.cpp
    test_db2_lobs.cpp)

target_link_libraries(halcyon_integration_tests
    PRIVATE halcyon::halcyon GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(halcyon_integration_tests
    PROPERTIES LABELS integration)

# C++20 coroutine-layer integration tests (skipped without coroutine support).
if(HALCYON_HAS_CXX20_COROUTINES)
    add_executable(halcyon_coro_integration_tests test_db2_coro.cpp)
    target_link_libraries(halcyon_coro_integration_tests
        PRIVATE halcyon::halcyon GTest::gtest_main)
    target_compile_features(halcyon_coro_integration_tests PRIVATE cxx_std_20)
    gtest_discover_tests(halcyon_coro_integration_tests
        PROPERTIES LABELS integration)
endif()
