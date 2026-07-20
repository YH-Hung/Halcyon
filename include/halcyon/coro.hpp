#pragma once

/// \file coro.hpp
/// \brief C++20 coroutine layer over the C++17 library (opt-in; deliberately
/// NOT part of the halcyon.hpp umbrella). Requires -std=c++20 in the
/// including TU; the compiled library itself remains C++17.
///
/// Threading contract (full version: docs/guide/coroutines.md):
///  - continuations resume on Halcyon executor worker threads — hop off for
///    long CPU-bound work;
///  - never call coro::syncWait on a worker thread (self-deadlock risk;
///    debug assert);
///  - one connection/transaction/stream is used by at most one thread at a
///    time across awaits (sequential, never concurrent — awaits provide the
///    happens-before edges IBM CLI requires for cross-thread handle use).
#include "halcyon/coro/detail/require_coroutines.hpp"
#include "halcyon/coro/task.hpp"
