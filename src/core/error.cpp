#include "halcyon/error.hpp"

namespace halcyon {

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:
            return "Ok";
        case ErrorCode::Connection:
            return "Connection";
        case ErrorCode::Timeout:
            return "Timeout";
        case ErrorCode::Constraint:
            return "Constraint";
        case ErrorCode::Syntax:
            return "Syntax";
        case ErrorCode::Deadlock:
            return "Deadlock";
        case ErrorCode::Transient:
            return "Transient";
        case ErrorCode::Mapping:
            return "Mapping";
        case ErrorCode::Pool:
            return "Pool";
        case ErrorCode::InvalidArgument:
            return "InvalidArgument";
        case ErrorCode::InvalidState:
            return "InvalidState";
        case ErrorCode::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

void throw_error(const Error& err) {
    switch (err.code) {
        case ErrorCode::Connection:
            throw ConnectionException(err);
        case ErrorCode::Timeout:
            throw TimeoutException(err);
        case ErrorCode::Constraint:
            throw ConstraintException(err);
        case ErrorCode::Deadlock:
        case ErrorCode::Transient:
            throw TransientException(err);
        case ErrorCode::Syntax:
            throw QueryException(err);
        case ErrorCode::Mapping:
            throw MappingException(err);
        case ErrorCode::Pool:
        case ErrorCode::InvalidArgument:
        case ErrorCode::InvalidState:
        case ErrorCode::Ok:
        case ErrorCode::Unknown:
            throw Exception(err);
    }
    throw Exception(err);
}

}  // namespace halcyon
