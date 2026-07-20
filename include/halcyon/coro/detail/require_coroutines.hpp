#pragma once

// halcyon/coro.hpp is the C++20 opt-in layer over the C++17 library. #error
// does NOT stop translation, so every coro header additionally gates its
// contents on HALCYON_HAS_CORO_SUPPORT — an unsupported TU gets exactly ONE
// clear diagnostic instead of a wall of <coroutine> template errors.
#if defined(__cpp_impl_coroutine)
#if defined(__has_include)
#if __has_include(<coroutine>)
#include <coroutine>
#endif
#else
#include <coroutine>
#endif
#endif

#if defined(__cpp_impl_coroutine) && defined(__cpp_lib_coroutine)
#define HALCYON_HAS_CORO_SUPPORT 1
#else
#define HALCYON_HAS_CORO_SUPPORT 0
#error "halcyon/coro.hpp requires C++20 coroutines; compile this TU with -std=c++20 or newer (compiler and standard library <coroutine> support required)"
#endif
