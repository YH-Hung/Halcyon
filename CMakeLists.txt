cmake_minimum_required(VERSION 3.20)
project(Halcyon VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(HALCYON_WITH_PROMETHEUS "Build prometheus-cpp metrics adapter" OFF)
option(HALCYON_WITH_OTEL "Build opentelemetry-cpp tracing adapter" OFF)
option(HALCYON_BUILD_TESTS "Build tests" ON)
option(HALCYON_BUILD_EXAMPLES "Build examples" OFF)
option(HALCYON_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(DB2CLI REQUIRED)

add_library(halcyon
    src/core/version.cpp
    src/core/error.cpp
    src/detail/cli/sqlstate.cpp)
add_library(halcyon::halcyon ALIAS halcyon)

target_include_directories(halcyon
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(halcyon PUBLIC cxx_std_17)
target_compile_options(halcyon PRIVATE -Wall -Wextra -Wpedantic)
if(HALCYON_WARNINGS_AS_ERRORS)
    target_compile_options(halcyon PRIVATE -Werror)
endif()

if(HALCYON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
