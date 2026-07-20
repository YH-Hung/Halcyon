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
" HALCYON_HAS_CXX20_COROUTINES)
set(CMAKE_REQUIRED_FLAGS "${_halcyon_saved_required_flags}")
set(CMAKE_CXX_STANDARD "${_halcyon_saved_cxx_standard}")

if(NOT HALCYON_HAS_CXX20_COROUTINES)
    message(STATUS
        "Halcyon: no usable C++20 coroutine support (floor: GCC 11+/Clang 14+/"
        "AppleClang 14+); coroutine tests and examples will be skipped")
endif()
