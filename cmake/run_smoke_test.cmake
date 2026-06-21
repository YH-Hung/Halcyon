# Install/find_package smoke test, run via `cmake -P`. Installs the already-built
# Halcyon into a scratch prefix, then configures, builds, and runs a standalone
# consumer that find_package(Halcyon)s it. Any failed step fails the test.
#
# Required -D vars: HALCYON_SOURCE_DIR, HALCYON_BUILD_DIR, DB2_CLIDRIVER_ROOT.
# Optional: DB2CLI_LIBRARY_DIR.

if(NOT HALCYON_SOURCE_DIR OR NOT HALCYON_BUILD_DIR)
    message(FATAL_ERROR "HALCYON_SOURCE_DIR and HALCYON_BUILD_DIR are required")
endif()

set(prefix "${HALCYON_BUILD_DIR}/smoke/install")
set(consumer_build "${HALCYON_BUILD_DIR}/smoke/consumer")
file(REMOVE_RECURSE "${prefix}" "${consumer_build}")

function(run_step desc)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "smoke step failed (${desc}): exit ${rc}")
    endif()
endfunction()

# Forward the build config to --install/--build so multi-config generators
# install and build the same artifact that was built (no-op on single-config).
set(config_args "")
if(SMOKE_CONFIG)
    set(config_args --config "${SMOKE_CONFIG}")
endif()

# 1. Install the built library + headers + package config into the scratch prefix.
run_step("install"
    ${CMAKE_COMMAND} --install "${HALCYON_BUILD_DIR}" ${config_args} --prefix "${prefix}")

# 2. Configure the consumer against the install tree.
run_step("consumer configure"
    ${CMAKE_COMMAND}
        -S "${HALCYON_SOURCE_DIR}/tests/smoke"
        -B "${consumer_build}"
        "-DCMAKE_PREFIX_PATH=${prefix}"
        "-DCMAKE_BUILD_TYPE=${SMOKE_CONFIG}"
        "-DDB2_CLIDRIVER_ROOT=${DB2_CLIDRIVER_ROOT}")

# 3. Build the consumer (link proves DB2::CLI propagation through the export).
run_step("consumer build"
    ${CMAKE_COMMAND} --build "${consumer_build}" ${config_args})

# 4. Run the consumer (proves headers + version + libdb2 load via RPATH).
set(exe "${consumer_build}/halcyon_smoke")
if(NOT EXISTS "${exe}" AND SMOKE_CONFIG)
    set(exe "${consumer_build}/${SMOKE_CONFIG}/halcyon_smoke")  # multi-config layout
endif()
run_step("consumer run" "${exe}")

message(STATUS "halcyon_install_smoke: OK")
