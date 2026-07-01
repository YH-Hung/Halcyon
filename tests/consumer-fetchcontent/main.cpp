// Consumer that obtains Halcyon via FetchContent (SOURCE_DIR -> the repo) and
// links halcyon::halcyon. No live DB: construct (do not connect) the real CLI
// driver to prove DB2::CLI propagated through add_subdirectory, and report the
// version.
#include <iostream>
#include <string>

#include <halcyon/halcyon.hpp>

int main() {
    const std::string v{halcyon::version()};
    std::cout << "halcyon (fetchcontent) " << v << "\n";

    auto driver = halcyon::detail::cli::make_db2_cli_driver();
    if (!driver) return 2;

    return v.empty() ? 3 : 0;
}
