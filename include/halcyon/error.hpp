#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace halcyon {

enum class ErrorCode {
    Ok,
    Connection,
    Timeout,
    Constraint,
    Syntax,
    Deadlock,
    Transient,
    Mapping,
    Pool,
    Unknown,
};

// Human-readable name for an ErrorCode (never null).
const char* to_string(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string sqlstate;    // raw 5-char SQLSTATE, e.g. "08001"
    int nativeError = 0;     // SQLCODE / native db2 code
    std::string message;     // diagnostic text
    bool retriable = false;  // transient class → drives auto-retry
};

class Exception : public std::runtime_error {
public:
    explicit Exception(Error err)
        : std::runtime_error(err.message), error_(std::move(err)) {}
    const Error& error() const noexcept { return error_; }

private:
    Error error_;
};

class ConnectionException : public Exception {
public:
    using Exception::Exception;
};
class QueryException : public Exception {
public:
    using Exception::Exception;
};
class ConstraintException : public Exception {
public:
    using Exception::Exception;
};
class TimeoutException : public Exception {
public:
    using Exception::Exception;
};
class TransientException : public Exception {
public:
    using Exception::Exception;
};
class MappingException : public Exception {
public:
    using Exception::Exception;
};

// Throws the exception subtype that corresponds to err.code. Never returns.
[[noreturn]] void throw_error(const Error& err);

}  // namespace halcyon
