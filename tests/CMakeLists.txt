include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

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
    unit/test_reflect.cpp
    unit/test_sqlstate.cpp
    unit/test_retry.cpp
    unit/test_pool.cpp)

target_link_libraries(halcyon_unit_tests
    PRIVATE halcyon::halcyon GTest::gtest_main Threads::Threads)

target_include_directories(halcyon_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/unit)

include(GoogleTest)
gtest_discover_tests(halcyon_unit_tests)

if(HALCYON_BUILD_INTEGRATION_TESTS)
    add_subdirectory(integration)
endif()
