# Standalone consumer that pulls Halcyon via FetchContent (pointed at the local
# source tree, so no network) and links halcyon::halcyon. Proves subdirectory
# consumption: the library target + usage requirements are available, and
# Halcyon's developer-only targets are NOT created in the consumer build.
cmake_minimum_required(VERSION 3.20)
project(halcyon_fetchcontent_consumer CXX)

if(NOT HALCYON_SOURCE_DIR)
    message(FATAL_ERROR "HALCYON_SOURCE_DIR is required")
endif()

include(FetchContent)
FetchContent_Declare(Halcyon SOURCE_DIR "${HALCYON_SOURCE_DIR}")
FetchContent_MakeAvailable(Halcyon)

# The guard must keep Halcyon's tests out of a consumer build.
if(TARGET halcyon_unit_tests)
    message(FATAL_ERROR
        "consumer build created Halcyon's test target; top-level guard missing")
endif()

add_executable(halcyon_fc_consumer main.cpp)
target_link_libraries(halcyon_fc_consumer PRIVATE halcyon::halcyon)

if(DB2CLI_LIBRARY_DIR)
    set_target_properties(halcyon_fc_consumer PROPERTIES
        BUILD_RPATH "${DB2CLI_LIBRARY_DIR}")
endif()
