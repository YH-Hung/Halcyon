# Standalone consumer project: built out-of-tree against an installed Halcyon to
# prove find_package(Halcyon) + halcyon::halcyon work end to end. Not part of the
# main build; driven by cmake/run_smoke_test.cmake.
cmake_minimum_required(VERSION 3.20)
project(halcyon_smoke_consumer CXX)

find_package(Halcyon CONFIG REQUIRED)

add_executable(halcyon_smoke main.cpp)
target_link_libraries(halcyon_smoke PRIVATE halcyon::halcyon)

# The static halcyon pulls in libdb2 transitively; bake its dir into the RPATH so
# the smoke binary can load it at runtime (DB2CLI_LIBRARY_DIR is set by the
# FindDB2CLI module that HalcyonConfig's find_dependency(DB2CLI) ran).
if(DB2CLI_LIBRARY_DIR)
    set_target_properties(halcyon_smoke PROPERTIES
        BUILD_RPATH "${DB2CLI_LIBRARY_DIR}")
endif()

if(HALCYON_EXPECTED_VERSION)
    target_compile_definitions(halcyon_smoke
        PRIVATE HALCYON_EXPECTED_VERSION="${HALCYON_EXPECTED_VERSION}")
endif()
