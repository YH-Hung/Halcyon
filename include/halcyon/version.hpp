#pragma once

#include <string_view>

namespace halcyon {

inline constexpr int version_major = 0;
inline constexpr int version_minor = 1;
inline constexpr int version_patch = 0;

// Returns the semantic version string, e.g. "0.1.0".
std::string_view version() noexcept;

}  // namespace halcyon
