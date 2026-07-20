cmake_minimum_required(VERSION 3.20)
project(Halcyon VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Define PROJECT_IS_TOP_LEVEL for CMake < 3.21 so we can keep the 3.20 floor.
# When Halcyon is consumed via add_subdirectory/FetchContent it is NOT top-level,
# and developer-only targets (tests, examples) must default OFF.
if(NOT DEFINED PROJECT_IS_TOP_LEVEL)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(PROJECT_IS_TOP_LEVEL TRUE)
    else()
        set(PROJECT_IS_TOP_LEVEL FALSE)
    endif()
endif()

# Single-source the version: generate include/halcyon/version.hpp from the CMake
# project version into the build tree. This generated header is the only one the
# compiler sees (the hand-maintained source header was removed).
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/halcyon/version.hpp"
    @ONLY)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(HALCYON_WITH_PROMETHEUS "Build prometheus-cpp metrics adapter" OFF)
option(HALCYON_WITH_OTEL "Build opentelemetry-cpp tracing adapter" OFF)
option(HALCYON_BUILD_TESTS "Build tests" ${PROJECT_IS_TOP_LEVEL})
option(HALCYON_BUILD_INTEGRATION_TESTS "Build Dockerized Db2 integration tests" OFF)
option(HALCYON_BUILD_EXAMPLES "Build examples" OFF)
option(HALCYON_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(HALCYON_BUILD_SMOKE_TEST "Add the install/find_package smoke test" ON)
option(HALCYON_BUILD_STRESS_TESTS "Build concurrency stress + perf suite" OFF)
set(HALCYON_SANITIZER "" CACHE STRING
    "Sanitizer for stress targets: thread|address|undefined (empty = none)")
set_property(CACHE HALCYON_SANITIZER PROPERTY STRINGS "" thread address undefined)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(HalcyonCoroutineSupport)
find_package(DB2CLI REQUIRED)

# Bake the Db2 CLI driver lib dir into every binary's RPATH so executables
# (tests, examples) find libdb2 without LD_LIBRARY_PATH/DYLD_LIBRARY_PATH. This
# must be set before any target is created. The halcyon library is static, so an
# RPATH on it alone would not propagate to consuming executables.
if(DB2CLI_LIBRARY_DIR)
    list(APPEND CMAKE_BUILD_RPATH "${DB2CLI_LIBRARY_DIR}")
    list(APPEND CMAKE_INSTALL_RPATH "${DB2CLI_LIBRARY_DIR}")
endif()

add_library(halcyon
    src/core/version.cpp
    src/core/error.cpp
    src/detail/cli/sqlstate.cpp
    src/detail/cli/db2_cli_driver.cpp
    src/observability/stderr_logger.cpp)
add_library(halcyon::halcyon ALIAS halcyon)

target_include_directories(halcyon
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(halcyon PUBLIC cxx_std_17)
target_compile_options(halcyon PRIVATE -Wall -Wextra -Wpedantic)
if(HALCYON_WARNINGS_AS_ERRORS)
    target_compile_options(halcyon PRIVATE -Werror)
endif()

# PUBLIC: halcyon is a static library, so the SQL* symbols from
# db2_cli_driver.cpp are resolved at the *consumer's* link step — the db2
# dependency must propagate via the exported usage requirements.
target_link_libraries(halcyon PUBLIC DB2::CLI)

# Optional observability adapters: compiled into the library ONLY when the option
# is ON and the (non-vendored) dependency is found. A missing package warns and
# skips rather than failing the build. The EFFECTIVE *_ADAPTER_ENABLED flags
# below (option AND package found) — not the requested options — drive both the
# compile definition and the exported package config's find_dependency, so an
# install built with the option ON but the package missing does not produce a
# config that demands a dependency that was never linked.
set(HALCYON_PROMETHEUS_ADAPTER_ENABLED FALSE)
set(HALCYON_OTEL_ADAPTER_ENABLED FALSE)

if(HALCYON_WITH_PROMETHEUS)
    find_package(prometheus-cpp CONFIG)
    if(prometheus-cpp_FOUND)
        target_sources(halcyon PRIVATE src/observability/prometheus_adapter.cpp)
        target_link_libraries(halcyon PUBLIC prometheus-cpp::core)
        target_compile_definitions(halcyon PUBLIC HALCYON_WITH_PROMETHEUS)
        set(HALCYON_PROMETHEUS_ADAPTER_ENABLED TRUE)
        message(STATUS "Halcyon: prometheus-cpp metrics adapter enabled")
    else()
        message(WARNING
            "HALCYON_WITH_PROMETHEUS=ON but prometheus-cpp not found; skipping adapter")
    endif()
endif()

if(HALCYON_WITH_OTEL)
    find_package(opentelemetry-cpp CONFIG)
    if(opentelemetry-cpp_FOUND)
        target_sources(halcyon PRIVATE src/observability/otel_adapter.cpp)
        target_link_libraries(halcyon PUBLIC
            opentelemetry-cpp::api opentelemetry-cpp::trace opentelemetry-cpp::common)
        target_compile_definitions(halcyon PUBLIC HALCYON_WITH_OTEL)
        set(HALCYON_OTEL_ADAPTER_ENABLED TRUE)
        message(STATUS "Halcyon: opentelemetry-cpp tracing adapter enabled")
    else()
        message(WARNING
            "HALCYON_WITH_OTEL=ON but opentelemetry-cpp not found; skipping adapter")
    endif()
endif()

if(DB2CLI_LIBRARY_DIR)
    set_target_properties(halcyon PROPERTIES
        BUILD_RPATH "${DB2CLI_LIBRARY_DIR}"
        INSTALL_RPATH "${DB2CLI_LIBRARY_DIR}")
endif()

# ---- Install + package config: find_package(Halcyon) -> halcyon::halcyon ----
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(HALCYON_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/Halcyon")

install(TARGETS halcyon EXPORT halcyonTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Public headers, including the detail/cli and observability subtrees.
install(DIRECTORY include/halcyon
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

# The generated version header lives in the build tree; install it alongside the
# hand-written public headers.
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/halcyon/version.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/halcyon)

install(EXPORT halcyonTargets
    FILE HalcyonTargets.cmake
    NAMESPACE halcyon::
    DESTINATION ${HALCYON_INSTALL_CMAKEDIR})

configure_package_config_file(
    cmake/HalcyonConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/HalcyonConfig.cmake"
    INSTALL_DESTINATION ${HALCYON_INSTALL_CMAKEDIR})

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/HalcyonConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion)

# Ship FindDB2CLI.cmake next to the config so find_dependency(DB2CLI) resolves
# the (link-time) db2 dependency in a downstream build.
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/HalcyonConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/HalcyonConfigVersion.cmake"
    cmake/FindDB2CLI.cmake
    DESTINATION ${HALCYON_INSTALL_CMAKEDIR})

if(HALCYON_BUILD_TESTS)
    enable_testing()

    add_test(NAME halcyon_db2_cli_include_isolation
        COMMAND ${CMAKE_COMMAND}
            -DHALCYON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DHALCYON_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DDB2CLI_INCLUDE_DIR=${DB2CLI_INCLUDE_DIR}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_db2_cli_include_isolation.cmake)

    add_subdirectory(tests)

    # Install smoke test: install to a scratch prefix, then configure+build+run a
    # standalone consumer that find_package(Halcyon)s it. Proves the exported
    # target, header install, version file, and DB2 dependency propagation.
    if(HALCYON_BUILD_SMOKE_TEST)
        add_test(NAME halcyon_install_smoke
            COMMAND ${CMAKE_COMMAND}
                -DHALCYON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DHALCYON_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DDB2_CLIDRIVER_ROOT=${DB2_CLIDRIVER_ROOT}
                -DDB2CLI_LIBRARY_DIR=${DB2CLI_LIBRARY_DIR}
                -DSMOKE_CONFIG=$<CONFIG>
                -DHALCYON_EXPECTED_VERSION=${PROJECT_VERSION}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_smoke_test.cmake)

        add_test(NAME halcyon_fetchcontent_smoke
            COMMAND ${CMAKE_COMMAND}
                -DHALCYON_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DHALCYON_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DDB2_CLIDRIVER_ROOT=${DB2_CLIDRIVER_ROOT}
                -DSMOKE_CONFIG=$<CONFIG>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_fetchcontent_test.cmake)
    endif()
endif()

if(HALCYON_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
