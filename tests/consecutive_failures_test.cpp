#include "consecutive_failures.h"

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        uint32_t failures = 0;
        for (uint32_t i = 1; i <= sfr::consecutive_failure_limit; ++i)
            require(sfr::failure_within_limit(failures), "failures up to the limit are tolerated");
        require(!sfr::failure_within_limit(failures), "one more failure than the limit is not");
        require(!sfr::failure_within_limit(failures), "and it stays refused");
        failures = 0;  // a success
        for (uint32_t i = 1; i <= 3; ++i) require(sfr::failure_within_limit(failures, 3), "a custom limit is tolerated");
        require(!sfr::failure_within_limit(failures, 3), "a custom limit is exceeded");
        failures = 0;
        require(sfr::failure_within_limit(failures, 3), "a success starts the count again");
        std::cout << "consecutive failures tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
