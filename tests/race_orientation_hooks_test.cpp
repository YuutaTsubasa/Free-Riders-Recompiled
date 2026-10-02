#include "ppc_recomp_shared.h"
#include "diagnostic_hooks.h"
#include <bit>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace harness {
using Hook = void (*)(PPCContext&, uint8_t*);
Hook orientation_hook = nullptr;
unsigned calls = 0;
float yaw = 0.75f;
constexpr uint32_t direction = 0x10000000;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

#undef SFR_CONCURRENT_HOOK
#define SFR_CONCURRENT_HOOK(name) PPC_FUNC(name); \
    static const bool name##_registered = (harness::orientation_hook = name, true); PPC_FUNC(name)
#include "../src/race_orientation_hooks.cpp"

namespace sfr { GuestMemory* active_memory = nullptr; }

PPC_FUNC(__imp__sub_8236DD60) {
    ++harness::calls;
    const auto component = [&](uint32_t offset) {
        return std::bit_cast<float>(sfr::active_memory->load<uint32_t>(ctx.r4.u32 + offset));
    };
    const float x = component(0), y = component(4), z = component(8);
    // Recorded failing input is (0,0,0,1). The title divides the dot
    // product by the vector lengths before taking acos: 0/0 poisons yaw.
    harness::yaw += std::acos(z / std::sqrt(x*x + y*y + z*z));
}

int main() {
    try {
        sfr::GuestMemory memory;
        memory.map(harness::direction, 0x1000);
        sfr::active_memory = &memory;
        PPCContext ctx;
        ctx.r3.u64 = 1;
        ctx.r4.u64 = harness::direction;
        ctx.lr = 0x82372AD4;
        auto call = harness::orientation_hook ? harness::orientation_hook : __imp__sub_8236DD60;
        auto set = [&](float x, float y, float z, float w) {
            const float values[]{x,y,z,w};
            for (unsigned i=0;i<4;++i)
                memory.store<uint32_t>(harness::direction+4*i, std::bit_cast<uint32_t>(values[i]));
        };
        set(0,0,0,1);
        call(ctx, memory.base());
        harness::require(harness::yaw == 0.75f && harness::calls == 0,
                         "uninitialized channel heading must preserve the rider orientation");
        set(-0.0f,0,-0.0f,0);
        call(ctx, memory.base());
        harness::require(harness::yaw == 0.75f && harness::calls == 0,
                         "signed zero and homogeneous coordinate must not create a direction");
        set(-0.999112f,0.00933416f,0.016346f,1);
        call(ctx, memory.base());
        harness::require(harness::calls == 1 && std::isfinite(harness::yaw),
                         "valid recorded channel direction must use original orientation update");
        set(0,1,0,1);
        call(ctx, memory.base());
        harness::require(harness::calls == 2, "vertical direction must not be mistaken for zero");
        set(1,0,0,1);
        call(ctx, memory.base());
        harness::require(harness::calls == 3, "horizontal direction must use original update");
        std::cout << "race orientation hook tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
