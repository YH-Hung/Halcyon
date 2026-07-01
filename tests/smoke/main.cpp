// Minimal consumer of an installed Halcyon. Forces compilation against the
// installed headers and linking against the static lib + its db2 dependency. No
// live DB connection — it only checks that the installed library is usable and
// reports the version baked into the linked lib.
#include <iostream>
#include <string>

#include <halcyon/halcyon.hpp>

int main() {
    const std::string v{halcyon::version()};
    std::cout << "halcyon " << v << "\n";

    // Construct (but do not connect) the real CLI driver: this pulls in the
    // db2-linked object file, so the link proves DB2::CLI propagation, and
    // running it loads libdb2 (proves the RPATH) by allocating an env handle.
    auto driver = halcyon::detail::cli::make_db2_cli_driver();
    if (!driver) return 2;

#ifdef HALCYON_EXPECTED_VERSION
    return v == HALCYON_EXPECTED_VERSION ? 0 : 3;
#else
    return v.empty() ? 4 : 0;
#endif
}
