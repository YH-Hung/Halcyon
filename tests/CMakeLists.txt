include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(halcyon_unit_tests
    unit/test_version.cpp)

target_link_libraries(halcyon_unit_tests
    PRIVATE halcyon::halcyon GTest::gtest_main)

target_include_directories(halcyon_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit)

include(GoogleTest)
gtest_discover_tests(halcyon_unit_tests)
