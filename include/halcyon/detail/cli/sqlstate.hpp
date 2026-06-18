#pragma once

#include <string_view>

#include "halcyon/error.hpp"

namespace halcyon::detail::cli {

struct Classification {
    ErrorCode code = ErrorCode::Unknown;
    bool retriable = false;
};

// Maps a Db2 SQLSTATE (and native SQLCODE) to a Halcyon ErrorCode per the spec.
Classification classify_sqlstate(std::string_view sqlstate, int nativeError) noexcept;

}  // namespace halcyon::detail::cli
