# Locates the IBM Db2 CLI driver (headers + db2 shared library).
# Result: imported target DB2::CLI, plus DB2CLI_FOUND.

if(NOT DB2_CLIDRIVER_ROOT)
    set(DB2_CLIDRIVER_ROOT "${CMAKE_SOURCE_DIR}/third_party/clidriver"
        CACHE PATH "Root of the IBM Db2 CLI driver")
endif()

find_path(DB2CLI_INCLUDE_DIR
    NAMES sqlcli1.h
    HINTS "${DB2_CLIDRIVER_ROOT}/include"
    NO_DEFAULT_PATH)

find_library(DB2CLI_LIBRARY
    NAMES db2
    HINTS "${DB2_CLIDRIVER_ROOT}/lib"
    NO_DEFAULT_PATH)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DB2CLI
    REQUIRED_VARS DB2CLI_LIBRARY DB2CLI_INCLUDE_DIR)

if(DB2CLI_FOUND AND NOT TARGET DB2::CLI)
    add_library(DB2::CLI UNKNOWN IMPORTED)
    set_target_properties(DB2::CLI PROPERTIES
        IMPORTED_LOCATION "${DB2CLI_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DB2CLI_INCLUDE_DIR}")
    # Bake an RPATH so executables find libdb2 without LD_LIBRARY_PATH/DYLD_LIBRARY_PATH.
    set(DB2CLI_LIBRARY_DIR "${DB2_CLIDRIVER_ROOT}/lib" CACHE PATH "Db2 CLI lib dir")

    # macOS: the local libdb2 ships with a bare install name ("libdb2.dylib").
    # dyld does not search RPATH for bare dependent names, so any executable that
    # links (even transitively) against it fails to load. Rewrite the install
    # name to be @rpath-relative (idempotent) so the baked RPATH resolves it.
    if(APPLE)
        execute_process(
            COMMAND otool -D "${DB2CLI_LIBRARY}"
            OUTPUT_VARIABLE _db2_install_name
            ERROR_QUIET)
        if(NOT _db2_install_name MATCHES "@rpath/libdb2.dylib")
            find_program(INSTALL_NAME_TOOL NAMES install_name_tool)
            if(INSTALL_NAME_TOOL)
                execute_process(COMMAND "${INSTALL_NAME_TOOL}"
                    -id @rpath/libdb2.dylib "${DB2CLI_LIBRARY}")
                message(STATUS
                    "DB2CLI: rewrote install name of ${DB2CLI_LIBRARY} to @rpath/libdb2.dylib")
            else()
                message(WARNING
                    "DB2CLI: install_name_tool not found; libdb2 may not load via RPATH on macOS")
            endif()
        endif()
    endif()
endif()

mark_as_advanced(DB2CLI_INCLUDE_DIR DB2CLI_LIBRARY)
