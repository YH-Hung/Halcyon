# Detects whether the toolchain can compile the C++20 coroutine layer
# (halcyon/coro.hpp). Sets HALCYON_HAS_CXX20_COROUTINES. Documented floor:
# GCC 11+, Clang 14+, AppleClang 14+ — below it (or without <coroutine>),
# C++20 coroutine tests/examples are skipped with a notice; the C++17
# library build is unaffected.
include(CheckCXXSourceCompiles)

# try_compile propagates CMAKE_CXX_STANDARD and appends its -std flag AFTER
# CMAKE_REQUIRED_FLAGS, so the standard must be raised via the variable (a
# bare CMAKE_REQUIRED_FLAGS "-std=c++20" would be overridden by the project's
# global C++17 back to -std=c++17).
set(_halcyon_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
set(_halcyon_saved_cxx_standard "${CMAKE_CXX_STANDARD}")
set(CMAKE_REQUIRED_FLAGS "-std=c++20")
set(CMAKE_CXX_STANDARD 20)
check_cxx_source_compiles("
#include <coroutine>
#if !defined(__cpp_impl_coroutine) || !defined(__cpp_lib_coroutine)
#error coroutines unavailable
#endif
struct Fire {
  struct promise_type {
    Fire get_return_object() { return {}; }
    std::suspend_never initial_suspend() { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() {}
    void unhandled_exception() {}
  };
};
Fire f() { co_return; }
int main() { f(); return 0; }
" HALCYON_CORO_PROBE_OK)
set(CMAKE_REQUIRED_FLAGS "${_halcyon_saved_required_flags}")
set(CMAKE_CXX_STANDARD "${_halcyon_saved_cxx_standard}")

# The probe alone is not sufficient: a below-floor toolchain can compile the
# tiny probe yet still lack the full, correct coroutine support the layer
# relies on. Enforce the documented floor (GCC 11+, Clang 14+, AppleClang 14+)
# explicitly, so such a compiler is skipped rather than enabled. An unknown
# compiler that passes the probe is trusted (no version data to gate on).
set(_halcyon_coro_floor_ok TRUE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 11)
        set(_halcyon_coro_floor_ok FALSE)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        set(_halcyon_coro_floor_ok FALSE)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        set(_halcyon_coro_floor_ok FALSE)
    endif()
endif()

if(HALCYON_CORO_PROBE_OK AND _halcyon_coro_floor_ok)
    set(HALCYON_HAS_CXX20_COROUTINES TRUE)
else()
    set(HALCYON_HAS_CXX20_COROUTINES FALSE)
endif()

if(NOT HALCYON_HAS_CXX20_COROUTINES)
    if(HALCYON_CORO_PROBE_OK AND NOT _halcyon_coro_floor_ok)
        message(STATUS
            "Halcyon: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} is "
            "below the documented C++20 coroutine floor (GCC 11+/Clang 14+/"
            "AppleClang 14+); coroutine tests and examples will be skipped")
    else()
        message(STATUS
            "Halcyon: no usable C++20 coroutine support (floor: GCC 11+/Clang 14+/"
            "AppleClang 14+); coroutine tests and examples will be skipped")
    endif()
endif()
