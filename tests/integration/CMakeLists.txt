add_executable(halcyon_integration_tests
    test_db2_roundtrip.cpp)

target_link_libraries(halcyon_integration_tests
    PRIVATE halcyon::halcyon GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(halcyon_integration_tests
    PROPERTIES LABELS integration)
