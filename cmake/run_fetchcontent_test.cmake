# FetchContent/add_subdirectory consumption test, run via `cmake -P`. Configures,
# builds, and runs a consumer that FetchContent_MakeAvailable(Halcyon) against the
# local source tree. Any failed step fails the test.
#
# Required -D vars: HALCYON_SOURCE_DIR, HALCYON_BUILD_DIR, DB2_CLIDRIVER_ROOT.

if(NOT HALCYON_SOURCE_DIR OR NOT HALCYON_BUILD_DIR)
    message(FATAL_ERROR "HALCYON_SOURCE_DIR and HALCYON_BUILD_DIR are required")
endif()

set(consumer_build "${HALCYON_BUILD_DIR}/fetchcontent/consumer")
file(REMOVE_RECURSE "${consumer_build}")

function(run_step desc)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "fetchcontent step failed (${desc}): exit ${rc}")
    endif()
endfunction()

set(config_args "")
if(SMOKE_CONFIG)
    set(config_args --config "${SMOKE_CONFIG}")
endif()

run_step("consumer configure"
    ${CMAKE_COMMAND}
        -S "${HALCYON_SOURCE_DIR}/tests/consumer-fetchcontent"
        -B "${consumer_build}"
        "-DCMAKE_BUILD_TYPE=${SMOKE_CONFIG}"
        "-DHALCYON_SOURCE_DIR=${HALCYON_SOURCE_DIR}"
        "-DDB2_CLIDRIVER_ROOT=${DB2_CLIDRIVER_ROOT}")

run_step("consumer build"
    ${CMAKE_COMMAND} --build "${consumer_build}" ${config_args})

set(exe "${consumer_build}/halcyon_fc_consumer")
if(NOT EXISTS "${exe}" AND SMOKE_CONFIG)
    set(exe "${consumer_build}/${SMOKE_CONFIG}/halcyon_fc_consumer")
endif()
run_step("consumer run" "${exe}")

message(STATUS "halcyon_fetchcontent_smoke: OK")
