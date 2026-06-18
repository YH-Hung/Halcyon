#pragma once

#include <memory>

#include "halcyon/detail/cli/driver.hpp"

namespace halcyon::detail::cli {

// Constructs the real IBM Db2 CLI driver. Defined in db2_cli_driver.cpp, the only
// translation unit that includes sqlcli1.h. Callers never see CLI types.
std::unique_ptr<ICliDriver> make_db2_cli_driver();

}  // namespace halcyon::detail::cli
