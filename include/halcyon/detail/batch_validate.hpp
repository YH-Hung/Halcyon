#pragma once

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "halcyon/detail/cli/driver.hpp"
#include "halcyon/error.hpp"
#include "halcyon/result.hpp"

namespace halcyon::detail {

// Validates that a batch is rectangular and each column is type-homogeneous,
// ignoring NULLs. Portable (operates on cli::Value variants, no CLI types), so
// it is unit-testable against MockCliDriver. Caller guarantees `rows` non-empty.
inline Result<void> validate_batch_rows(
    const std::vector<std::vector<cli::Value>>& rows) {
    constexpr std::size_t kUnset = static_cast<std::size_t>(-1);
    const std::size_t ncols = rows.front().size();
    std::vector<std::size_t> colType(ncols, kUnset);  // variant index per column
    for (std::size_t r = 0; r < rows.size(); ++r) {
        if (rows[r].size() != ncols) {
            Error e;
            e.code = ErrorCode::Mapping;
            e.message = "batch row " + std::to_string(r) + " has " +
                        std::to_string(rows[r].size()) +
                        " columns, expected " + std::to_string(ncols);
            return e;
        }
        for (std::size_t c = 0; c < ncols; ++c) {
            const auto& v = rows[r][c];
            if (std::holds_alternative<cli::Null>(v)) continue;  // null: any type
            const std::size_t idx = v.index();
            if (colType[c] == kUnset) {
                colType[c] = idx;
            } else if (colType[c] != idx) {
                Error e;
                e.code = ErrorCode::Mapping;
                e.message = "batch column " + std::to_string(c) +
                            " mixes value types (first at row " +
                            std::to_string(r) + ")";
                return e;
            }
        }
    }
    return Result<void>();
}

}  // namespace halcyon::detail
