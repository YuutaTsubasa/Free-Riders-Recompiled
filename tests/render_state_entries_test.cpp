#include "render_state_entries.h"
#include <iostream>
#include <stdexcept>

int main() {
    try {
        constexpr uint32_t setters[] = {0x824E6A08,0x824E6EC8,0x824E6E68,0x824E70D0,0x824E7140,0x824E7110,0x824E69A8,
                                       0x824E6A40,0x824E6B60,0x824E6BF0,0x824E8248,0x824E83F0};
        for (uint32_t setter : setters) {
            if (!sfr::is_native_render_state_entry(setter))
                throw std::runtime_error("complete original render-state entry lost");
            for (uint32_t offset : {1u, 4u})
                if (sfr::is_native_render_state_entry(setter + offset) || sfr::is_native_render_state_entry(setter - offset))
                    throw std::runtime_error("interior or adjacent entry is not a complete audited setter");
        }
        for (uint32_t partial : {0u, 0xffffffffu, 0x824E9218u, 0x824F4220u, 0x824ED770u})
            if (sfr::is_native_render_state_entry(partial))
                throw std::runtime_error("unrelated or partial bridge is not included");
        std::cout << "Original render-state entry checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
