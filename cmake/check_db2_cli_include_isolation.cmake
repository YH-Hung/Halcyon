# Compiles the real Db2 CLI seam with an ambient sqlext.h ahead of system search
# paths. The seam must rely only on the Db2 driver's own ODBC extension header.

foreach(required_var
        HALCYON_SOURCE_DIR
        HALCYON_BUILD_DIR
        DB2CLI_INCLUDE_DIR
        CMAKE_CXX_COMPILER)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

set(work_dir "${HALCYON_BUILD_DIR}/db2-cli-include-isolation")
set(fake_include_dir "${work_dir}/ambient-odbc")
file(MAKE_DIRECTORY "${fake_include_dir}")
file(WRITE "${fake_include_dir}/sqlext.h"
    "#error ambient sqlext.h was included\n")

set(source_file "${HALCYON_SOURCE_DIR}/src/detail/cli/db2_cli_driver.cpp")
set(object_file "${work_dir}/db2_cli_driver.o")

execute_process(
    COMMAND
        ${CMAKE_COMMAND} -E env "CPATH=${fake_include_dir}"
        "${CMAKE_CXX_COMPILER}"
        "-I${HALCYON_SOURCE_DIR}/include"
        "-I${HALCYON_BUILD_DIR}/generated/include"
        "-isystem" "${DB2CLI_INCLUDE_DIR}"
        -std=c++17
        -Wall -Wextra -Wpedantic -Werror
        -c "${source_file}"
        -o "${object_file}"
    RESULT_VARIABLE compile_result
    OUTPUT_VARIABLE compile_stdout
    ERROR_VARIABLE compile_stderr)

if(NOT compile_result EQUAL 0)
    message(FATAL_ERROR
        "Db2 CLI include isolation check failed.\n"
        "stdout:\n${compile_stdout}\n"
        "stderr:\n${compile_stderr}")
endif()

message(STATUS "halcyon_db2_cli_include_isolation: OK")
