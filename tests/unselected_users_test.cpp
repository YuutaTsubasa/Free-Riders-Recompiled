#include "unselected_users.h"
#include "guest_memory.h"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        for (uint32_t slot = 0; slot != 4; ++slot)
            if (sfr::unselected_user_signin_state(slot) != 0)
                throw std::runtime_error("empty game slot must not claim local or Live sign-in");
        for (uint32_t unsupported : {4u, 255u, 0xfffffffeu, 0xffffffffu}) {
            bool rejected = false;
            try { (void)sfr::unselected_user_signin_state(unsupported); }
            catch (const sfr::RuntimeStop&) { rejected = true; }
            if (!rejected) throw std::runtime_error("unknown user index silently treated as a valid empty slot");
        }
        std::cout << "Unselected user query checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
