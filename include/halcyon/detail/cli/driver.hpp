#pragma once

#include <cstdint>
#include <string>

#include "halcyon/result.hpp"

namespace halcyon::detail::cli {

// Opaque handle to a physical connection owned by a driver implementation.
// 0 is reserved as the invalid handle.
enum class ConnectionHandle : std::uint64_t { invalid = 0 };

struct ConnectionParams {
    std::string connectionString;
};

// Thin seam over a Db2 CLI driver. The only interface the core/pool depend on
// for establishing and checking physical connections. Implementations must be
// safe to call from the thread that currently owns a given handle.
class ICliDriver {
public:
    virtual ~ICliDriver() = default;

    // Establishes a physical connection. On success returns a non-invalid handle.
    virtual Result<ConnectionHandle> connect(const ConnectionParams& params) = 0;

    // Releases a previously returned handle. Idempotent for already-closed handles.
    virtual Result<void> disconnect(ConnectionHandle handle) = 0;

    // Lightweight liveness probe (e.g. validation query) for the handle.
    virtual Result<bool> isAlive(ConnectionHandle handle) = 0;
};

}  // namespace halcyon::detail::cli
